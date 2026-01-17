#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <HardwareSerial.h>
#include <vector>

// --- CONFIGURAÇÕES DE REDE E GOOGLE ---
const char* ssid = "nome do wifi";
const char* password = "Senha do wifi";
const char* google_script_id = "AKfycbxZo3zrlLdSPQdehR3EhI_YTTnUPxXkcm8txwEH-26ZzgtHZL9yXUsA9xgWC8CYm82GNA";

// --- Configurações de Hardware BL0942 (ESP32-C6) ---
#define BL0942_RX_PIN 11
#define BL0942_TX_PIN 13
HardwareSerial blSerial(1);

// Endereços e Constantes
#define REG_I_WAVE      0x01
#define REG_MODE        0x19
#define REG_USR_WRPROT  0x1D

#define K_VOLTAGE 0.00041248f
#define K_CURRENT 0.0000034690f
#define K_POWER   0.0095800f
#define K_ENERGY  0.001617f

static const int N_WAVE = 1024;

// Estrutura de Dados
struct BL0942Data {
  float current = 0, voltage = 0, power = 0, total_energy = 0, frequency = 0;
  float power_apparent = 0, power_reactive = 0, power_factor = 0, phase_angle = 0;

  // --- NOVOS (somente diagnóstico/print) ---
  uint16_t raw_freq = 0;
  int32_t  raw_watt_signed = 0;
  uint16_t wave_pts = 0;
  uint32_t wave_dt_ms = 0;
  float    fs_sps = 0;
  float    f_wave = 0; // frequência estimada pela waveform

  std::vector<int32_t> current_waveform;
  BL0942Data() : current_waveform(N_WAVE) {}
};

// --- FUNÇÕES AUXILIARES ---
void setup_wifi() {
  Serial.print("Conectando ao WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFalha ao conectar no WiFi. O loop tentará novamente.");
  }
}

byte calculatePacketChecksum(const byte* buffer, byte frameHeader) {
  uint16_t sum = frameHeader;
  for (int i = 0; i < 22; i++) sum += buffer[i];
  return (byte)~(sum & 0xFF);
}

void writeRegister(byte address, uint32_t value) {
  byte frameHeader = 0xA8;
  byte data[3] = {
    (byte)(value & 0xFF),
    (byte)((value >> 8) & 0xFF),
    (byte)((value >> 16) & 0xFF)
  };
  uint16_t sum = frameHeader + address + data[0] + data[1] + data[2];
  byte checksum = (byte)~(sum & 0xFF);

  blSerial.write(frameHeader);
  blSerial.write(address);
  blSerial.write(data, 3);
  blSerial.write(checksum);
  delay(10);
}

// --- Estima frequência pela waveform (opcional, só pra diagnóstico) ---
float estimateFrequencyFromWave(const std::vector<int32_t>& x, int n, float fs) {
  if (n < 50 || fs <= 1) return 0;

  // remove DC (média)
  double mean = 0;
  int32_t maxabs = 0;
  for (int i = 0; i < n; i++) {
    mean += x[i];
    int32_t a = abs(x[i]);
    if (a > maxabs) maxabs = a;
  }
  mean /= n;

  // histerese p/ evitar múltiplos cruzamentos por ruído
  int32_t thr = (int32_t)(0.10 * maxabs); // 10% do pico
  if (thr < 5) thr = 5;

  // conta cruzamentos "subindo" (negativo -> positivo) com histerese
  int state = 0; // -1 abaixo, +1 acima, 0 indefinido
  int rises = 0;

  for (int i = 0; i < n; i++) {
    int32_t v = (int32_t)(x[i] - mean);

    int newState = state;
    if (v > thr) newState = +1;
    else if (v < -thr) newState = -1;

    if (state == -1 && newState == +1) rises++;
    state = newState;
  }

  float T = (float)n / fs;   // duração da janela
  if (T <= 0) return 0;
  return rises / T;          // rises ~ ciclos (subidas por ciclo)
}

// --- LEITURA DO SENSOR ---
bool readBL0942Packet(BL0942Data &data) {
  while (blSerial.available()) blSerial.read();
  byte readCmd[] = {0x58, 0xAA};
  blSerial.write(readCmd, 2);

  byte buffer[23];
  blSerial.setTimeout(50);
  if (blSerial.readBytes(buffer, 23) != 23) return false;
  if (buffer[0] != 0x55 || calculatePacketChecksum(buffer, 0x58) != buffer[22]) return false;

  uint32_t raw_i_rms = ((uint32_t)buffer[3] << 16) | ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[1];
  uint32_t raw_v_rms = ((uint32_t)buffer[6] << 16) | ((uint32_t)buffer[5] << 8) | (uint32_t)buffer[4];

  int32_t raw_watt_signed = ((int32_t)buffer[12] << 24) | ((int32_t)buffer[11] << 16) | ((int32_t)buffer[10] << 8);
  raw_watt_signed >>= 8;

  uint16_t raw_freq = ((uint16_t)buffer[17] << 8) | (uint16_t)buffer[16];

  // salva brutos (NOVO)
  data.raw_freq = raw_freq;
  data.raw_watt_signed = raw_watt_signed;

  // escala
  data.voltage = raw_v_rms * K_VOLTAGE;
  data.current = raw_i_rms * K_CURRENT;

  // freq pelo registrador
  data.frequency = (raw_freq > 0) ? (1000000.0f / raw_freq) : 0.0f;

  // S, PF, P
  data.power_apparent = data.voltage * data.current;
  float P_chip = raw_watt_signed * K_POWER;

  data.power_factor = (data.power_apparent > 0) ? (P_chip / data.power_apparent) : 0;
  if (data.power_factor > 1.0f) data.power_factor = 1.0f;
  if (data.power_factor < -1.0f) data.power_factor = -1.0f;

  data.power = data.power_apparent * data.power_factor;

  return true;
}

bool readWaveform(BL0942Data &data) {
  std::fill(data.current_waveform.begin(), data.current_waveform.end(), 0);

  uint32_t t0 = micros();
  int i = 0;

  for (i = 0; i < N_WAVE; i++) {
    byte cmd[] = {0x58, REG_I_WAVE};
    blSerial.write(cmd, 2);

    unsigned long timeout = micros();
    while (blSerial.available() < 4) {
      if (micros() - timeout > 3000) {
        break;
      }
    }
    if (blSerial.available() < 4) break;

    byte b1 = blSerial.read();
    byte b2 = blSerial.read();
    byte b3 = blSerial.read();
    blSerial.read(); // checksum (ignorado)

    int32_t wave = (uint32_t)b1 | ((uint32_t)b2 << 8) | ((uint32_t)b3 << 16);
    if (wave & 0x800000) wave |= 0xFF000000;

    data.current_waveform[i] = wave;
  }

  uint32_t dt_us = micros() - t0;
  data.wave_pts = (uint16_t)i;
  data.wave_dt_ms = dt_us / 1000;
  data.fs_sps = (dt_us > 0) ? (1000000.0f * i / (float)dt_us) : 0.0f;

  // freq estimada da waveform (só diagnóstico)
  data.f_wave = estimateFrequencyFromWave(data.current_waveform, data.wave_pts, data.fs_sps);

  return (i > 0);
}

// --- ENVIO PARA GOOGLE SHEETS (ENVIO IGUAL AO ANTIGO) ---
void sendToGoogle(BL0942Data &data) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi caiu, tentando reconectar...");
    WiFi.reconnect();
    return;
  }

  HTTPClient http;
  String url = "https://script.google.com/macros/s/" + String(google_script_id) + "/exec";

  String jsonOutput;
  if (!jsonOutput.reserve(16384)) {
    Serial.println("Erro: Falha ao reservar memória para JSON!");
    return;
  }

  if (isnan(data.voltage)) data.voltage = 0.0;
  if (isnan(data.current)) data.current = 0.0;
  if (isnan(data.power)) data.power = 0.0;
  if (isnan(data.power_factor)) data.power_factor = 0.0;
  if (isnan(data.frequency)) data.frequency = 0.0;

  // >>> ENVIO IGUAL AO SEU ORIGINAL (SEM CAMPOS NOVOS) <<<
  jsonOutput = "{";
  jsonOutput += "\"v\":" + String(data.voltage, 2) + ",";
  jsonOutput += "\"i\":" + String(data.current, 3) + ",";
  jsonOutput += "\"p\":" + String(data.power, 2) + ",";
  jsonOutput += "\"pf\":" + String(data.power_factor, 2) + ",";
  jsonOutput += "\"f\":" + String(data.frequency, 1) + ",";
  jsonOutput += "\"wave\":\"";

  for (int i = 0; i < N_WAVE; i++) {
    jsonOutput += String(data.current_waveform[i]);
    if (i < N_WAVE - 1) jsonOutput += ",";
  }
  jsonOutput += "\"}";

  Serial.print("Tamanho do Payload: ");
  Serial.println(jsonOutput.length());

  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int httpResponseCode = http.POST(jsonOutput);

  Serial.printf("Status Code: %d\n", httpResponseCode);
  if (httpResponseCode > 0) {
    // (mantém como estava: não precisa imprimir body se você não quiser)
    // String payload = http.getString();
    // Serial.println("Resposta: " + payload);
  } else {
    Serial.printf("Erro HTTP: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

// --- SETUP E LOOP ---
void setup() {
  Serial.begin(250000);
  delay(1000);
  Serial.println("\n--- INICIANDO SISTEMA ---");

  setup_wifi();

  blSerial.begin(4800, SERIAL_8N1, BL0942_RX_PIN, BL0942_TX_PIN);
  Serial.println("Configurando BL0942...");
  writeRegister(REG_USR_WRPROT, 0x55);
  writeRegister(REG_MODE, 0x0003A7);
  delay(100);
  blSerial.end();

  blSerial.begin(38400, SERIAL_8N1, BL0942_RX_PIN, BL0942_TX_PIN);
  Serial.println("Monitoramento iniciado.");
}

void loop() {
  BL0942Data d;

  bool packetSuccess = readBL0942Packet(d);
  if (!packetSuccess) {
    Serial.println("Erro na leitura do Pacote BL0942.");
    delay(1000);
    return;
  }

  bool waveSuccess = readWaveform(d);

  Serial.printf(
    "V: %.2fV | I: %.3fA | P: %.2fW | FP: %.2f | F: %.2fHz | raw_freq=%u | WavePts:%u | dt:%ums | fs:%.1f SPS | f_wave:%.2fHz\n",
    d.voltage, d.current, d.power, d.power_factor, d.frequency,
    d.raw_freq, d.wave_pts, d.wave_dt_ms, d.fs_sps, d.f_wave
  );

  // envia como antes (sempre 1024 pontos no JSON)
  if (waveSuccess && d.wave_pts == 1024) {
    sendToGoogle(d);
  } else {
    Serial.println("Waveform parcial/timeout — não enviando pra não bagunçar.");
  }

  delay(1000);
}

