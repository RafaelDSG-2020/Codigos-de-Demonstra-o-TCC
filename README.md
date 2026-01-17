````markdown
# Dashboard e Firmware (ESP32-C6 + BL0942 + Google Sheets) — Documentação do TCC

Este documento descreve **dois códigos do TCC**:

1. **Firmware (ESP32-C6)**: lê grandezas elétricas e a forma de onda do BL0942 e envia para **Google Sheets** via **Google Apps Script** (HTTP POST).
2. **Dashboard (Python/Matplotlib)**: lê os dados do Google Sheets (CSV publicado) e exibe um painel com **tendências**, **forma de onda** e **FFT + harmônicas + THD₅**.

---

## 🔗 Link da planilha usada no projeto

- Planilha (visualização/edição):  
  https://docs.google.com/spreadsheets/d/1LQPQEdYMTuhcVRgOs2n9uCMaQbLgHsM3GOl6wgEn3FE/edit?usp=sharing

> Observação: para o dashboard Python funcionar, a planilha precisa estar **“Publicada na Web”** (para gerar o link CSV público). O código Python usa a URL *pubhtml* convertida para CSV.

---

# 1) Firmware do ESP32-C6 (BL0942 → Google Sheets)

## 1.1 Objetivo do firmware

O firmware executa um ciclo contínuo de:

1. Conectar ao Wi-Fi.
2. Configurar o BL0942 (registradores).
3. Ler **pacote de medições** (RMS e frequência) via UART.
4. Ler **1024 amostras** da forma de onda de corrente (registrador de waveform).
5. Calcular dados auxiliares de diagnóstico (Fs estimado, frequência pela waveform).
6. Enviar JSON via HTTP POST para um endpoint do **Google Apps Script** que grava na planilha.

---

## 1.2 Bibliotecas e por que são usadas

```cpp
#include <WiFi.h>          // conexão Wi-Fi
#include <HTTPClient.h>    // envio HTTP (POST) para Apps Script
#include <ArduinoJson.h>   // (está incluída, mas o JSON aqui é montado “na mão”)
#include <math.h>          // operações matemáticas
#include <HardwareSerial.h>// UART dedicada para o BL0942
#include <vector>          // armazenar waveform com tamanho fixo (1024)
````

> Nota: `ArduinoJson` está incluída, mas o payload JSON é montado via `String`. Ela pode ser usada no futuro para montar JSON de forma mais robusta.

---

## 1.3 Configurações de rede e Apps Script

```cpp
const char* ssid = "TP-Link_D9FF";
const char* password = "16069138";
const char* google_script_id = "AKfycbxZo3zrlLdSPQdehR3EhI_YTTnUPxXkcm8txwEH-26ZzgtHZL9yXUsA9xgWC8CYm82GNA";
```

* `ssid` e `password`: rede Wi-Fi para o ESP32.
* `google_script_id`: identificador do deployment do Apps Script. O endpoint final fica no formato:

  * `https://script.google.com/macros/s/<google_script_id>/exec`

---

## 1.4 UART e pinos do BL0942

```cpp
#define BL0942_RX_PIN 11
#define BL0942_TX_PIN 13
HardwareSerial blSerial(1);
```

* O BL0942 comunica por UART.
* `HardwareSerial(1)` usa a UART1 do ESP32-C6.
* RX/TX são definidos conforme o hardware do projeto.

---

## 1.5 Registradores e constantes de escala

```cpp
#define REG_I_WAVE      0x01
#define REG_MODE        0x19
#define REG_USR_WRPROT  0x1D
```

* `REG_I_WAVE`: leitura do ponto de waveform de corrente.
* `REG_MODE`: configura o modo de operação.
* `REG_USR_WRPROT`: desbloqueio para escrita (proteção).

Constantes de conversão (calibração/escala):

```cpp
#define K_VOLTAGE 0.00041248f
#define K_CURRENT 0.0000034690f
#define K_POWER   0.0095800f
#define K_ENERGY  0.001617f
```

Elas convertem valores brutos do chip para unidades reais:

* `tensão = raw_v_rms * K_VOLTAGE`
* `corrente = raw_i_rms * K_CURRENT`
* `potência_chip = raw_watt_signed * K_POWER`

---

## 1.6 Estrutura de dados (BL0942Data)

```cpp
struct BL0942Data {
  float current = 0, voltage = 0, power = 0, total_energy = 0, frequency = 0;
  float power_apparent = 0, power_reactive = 0, power_factor = 0, phase_angle = 0;

  // diagnóstico
  uint16_t raw_freq = 0;
  int32_t  raw_watt_signed = 0;
  uint16_t wave_pts = 0;
  uint32_t wave_dt_ms = 0;
  float    fs_sps = 0;
  float    f_wave = 0;

  std::vector<int32_t> current_waveform;
  BL0942Data() : current_waveform(N_WAVE) {}
};
```

Campos principais:

* `voltage`, `current`, `power`, `frequency`, `power_factor`.

Campos de **diagnóstico** (importantes para validar aquisição):

* `raw_freq`: valor bruto de frequência vindo do chip.
* `raw_watt_signed`: potência ativa bruta com sinal (pode indicar fluxo reverso).
* `wave_pts`: quantos pontos foram realmente capturados.
* `wave_dt_ms`: tempo total de captura em ms.
* `fs_sps`: amostras por segundo estimadas (Fs).
* `f_wave`: frequência estimada usando a waveform (checagem).

`current_waveform`:

* vetor com `1024` amostras (tipo `int32_t`), onde cada amostra é um valor bruto do BL0942.

---

## 1.7 Conexão Wi-Fi

```cpp
void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  ...
}
```

* Tenta conectar por até 20 tentativas (10 s no total, pois `delay(500)`).
* Se falhar, segue e o loop tenta novamente quando precisar enviar.

---

## 1.8 Checksum do protocolo (integridade do pacote)

```cpp
byte calculatePacketChecksum(const byte* buffer, byte frameHeader) {
  uint16_t sum = frameHeader;
  for (int i = 0; i < 22; i++) sum += buffer[i];
  return (byte)~(sum & 0xFF);
}
```

* Soma o `frameHeader` e os 22 bytes do pacote.
* Faz `~(sum & 0xFF)` para obter o checksum (complemento de 1 byte).
* Usado para validar se o pacote recebido está íntegro.

---

## 1.9 Escrita de registradores no BL0942

```cpp
void writeRegister(byte address, uint32_t value) {
  byte frameHeader = 0xA8;
  byte data[3] = { ... }; // 24 bits
  ...
  blSerial.write(frameHeader);
  blSerial.write(address);
  blSerial.write(data, 3);
  blSerial.write(checksum);
}
```

* `0xA8` é o header de escrita.
* Envia endereço + 3 bytes (24 bits) + checksum.
* Necessário para configurar o modo e destravar escrita.

---

## 1.10 Estimativa de frequência pela waveform (diagnóstico)

```cpp
float estimateFrequencyFromWave(const std::vector<int32_t>& x, int n, float fs)
```

O que faz:

1. Remove DC (média).
2. Calcula a amplitude máxima (`maxabs`) para criar uma **histerese**.
3. Define um limiar `thr` = 10% do pico (mínimo 5).
4. Conta cruzamentos **subindo** (de abaixo de `-thr` para acima de `+thr`).
5. Estima frequência como:

* `T = n / fs`
* `freq ≈ rises / T`

Por que existe:

* Serve para validar se a waveform capturada faz sentido em relação à frequência do registrador do chip.
* Ajuda a detectar problemas de amostragem ou timeout na leitura da waveform.

---

## 1.11 Leitura do pacote principal do BL0942 (RMS, potência, frequência)

```cpp
bool readBL0942Packet(BL0942Data &data)
```

Fluxo:

1. Limpa a UART.
2. Envia comando `{0x58, 0xAA}` para solicitar o pacote.
3. Lê `23 bytes`.
4. Valida:

   * `buffer[0] == 0x55` (header esperado)
   * checksum calculado confere com `buffer[22]`

Extração de dados brutos:

* `raw_i_rms` e `raw_v_rms`: 24 bits.
* `raw_watt_signed`: potência ativa com sinal (ajuste com shift para sinal).
* `raw_freq`: 16 bits.

Conversões:

* `voltage = raw_v_rms * K_VOLTAGE`
* `current = raw_i_rms * K_CURRENT`
* `frequency = 1000000 / raw_freq` (quando `raw_freq > 0`)

Cálculos elétricos derivados:

* `S (aparente) = V * I`
* `P_chip = raw_watt_signed * K_POWER`
* `PF = P_chip / S` (limitado em [-1, 1])
* `P = S * PF`

> Observação: o código calcula `P` coerente com `S` e `PF`. Isso reduz inconsistências caso `P_chip` tenha ruído.

---

## 1.12 Leitura da waveform (1024 pontos)

```cpp
bool readWaveform(BL0942Data &data)
```

O que acontece:

1. Zera o vetor `current_waveform`.
2. Marca `t0 = micros()` para medir tempo de aquisição.
3. Para `i = 0..1023`:

   * Envia `{0x58, REG_I_WAVE}`
   * Aguarda chegar 4 bytes (3 dados + checksum)
   * Lê 3 bytes e ignora checksum
   * Constrói amostra 24 bits e aplica sign-extend (sinal)

Ao final:

* `dt_us = micros() - t0`
* `wave_pts = i` (quantos pontos foram realmente capturados)
* `wave_dt_ms = dt_us / 1000`
* `fs_sps = (1e6 * i / dt_us)` (estimativa de Fs real)

E calcula:

* `f_wave = estimateFrequencyFromWave(...)`

Critério de sucesso:

* retorna `true` se capturou ao menos 1 ponto, mas o envio ao Google só acontece se:

  * `wave_pts == 1024`

> Isso evita “bagunçar” a planilha com waveform parcial.

---

## 1.13 Envio para Google Sheets via Apps Script

```cpp
void sendToGoogle(BL0942Data &data)
```

Condições:

* Se Wi-Fi cair, chama `WiFi.reconnect()` e sai.

Monta URL:

```cpp
String url = "https://script.google.com/macros/s/" + String(google_script_id) + "/exec";
```

Monta JSON (mesmo formato do seu envio original):

* Campos enviados:

  * `"v"` tensão
  * `"i"` corrente
  * `"p"` potência
  * `"pf"` fator de potência
  * `"f"` frequência
  * `"wave"` string com 1024 inteiros separados por vírgula

A waveform é enviada como **string**, porque:

* Facilita gravar em uma célula (ou colunas) do Sheets via Apps Script.
* Evita lidar com array JSON grande do lado do Apps Script.

HTTP:

* Content-Type: `application/json`
* Timeout: 15000 ms
* POST do payload
* Imprime status code para depuração

---

## 1.14 setup() e loop()

### setup()

1. Inicializa Serial (debug).
2. Conecta no Wi-Fi.
3. Inicializa UART do BL0942:

   * Começa em `4800` (modo de configuração)
   * Destrava escrita (`REG_USR_WRPROT = 0x55`)
   * Configura modo (`REG_MODE = 0x0003A7`)
   * Reinicia UART para `38400` (modo de operação)

### loop()

1. Cria `BL0942Data d`.
2. Lê pacote:

   * Se falhar, espera 1 s e tenta de novo.
3. Lê waveform.
4. Imprime diagnóstico completo:

   * V, I, P, PF, F, raw_freq, wave_pts, dt, fs_sps, f_wave
5. Só envia para o Google se:

   * `waveSuccess == true` **e** `wave_pts == 1024`
6. Delay de 1 s.

---

# 2) Dashboard Python (Google Sheets → gráficos + FFT + THD₅)

## 2.1 Objetivo do dashboard

O dashboard lê os dados da planilha (publicada em CSV) e cria uma visualização com:

* Tendência de:

  * tensão, corrente, potência, FP
* Forma de onda (tempo):

  * com normalização e referência senoidal
* FFT:

  * espectro até 350 Hz (ou Nyquist)
  * picos H1..H5 (60..300 Hz)
  * cálculo de THD₅

---

## 2.2 Fonte dos dados (CSV publicado)

O dashboard usa:

```python
URL_ORIGINAL = '.../pubhtml'
URL_CSV = URL_ORIGINAL.replace('/pubhtml', '/pub?output=csv')
```

> Ou seja: ele não usa o link “edit”, e sim a versão “publicada na web”.

---

## 2.3 Como ele interpreta a linha da planilha

A função `processar_pacote_exato()` interpreta a linha como:

* Coluna 0: timestamp/hora (string)
* Colunas 1..5: valores escalares (V, I, P, PF, F)
* Onda:

  * Caso A: várias colunas a partir da 6
  * Caso B: uma string na coluna 6 com “a,b,c...”
* Ajusta para exatamente 1024 pontos (corta ou preenche com zeros)

---

## 2.4 Parte de DSP (FFT)

O FFT do dashboard:

* remove DC
* aplica janela Hann
* usa rFFT
* calcula magnitude aproximada (amplitude pico)

Depois:

* busca picos em torno de 60/120/180/240/300 com banda ± `BANDA_HZ`
* calcula THD₅:

  * `sqrt(H2²+H3²+H4²+H5²)/H1 * 100`

---

## 2.5 Loop de atualização

O dashboard roda em loop infinito:

* lê a planilha
* atualiza os gráficos
* espera 3 s

Interromper com `Ctrl+C` (ou parar a célula no notebook).

---

# 3) Integração entre os dois códigos (fluxo do TCC)

1. **ESP32-C6 + BL0942** mede grandezas e captura **waveform (1024 pts)**.
2. ESP32 envia um **JSON** para o **Apps Script**.
3. Apps Script grava na **planilha**.
4. O **Python** lê o CSV público dessa planilha e exibe o dashboard:

   * tendências, waveform e FFT/THD.

---

## ✅ Observação importante (consistência entre firmware e dashboard)

* O firmware envia `"wave"` como **string CSV** (ex.: `"1,2,3,..."`).
* O dashboard aceita exatamente esse formato no “Caso B” (string com valores separados por vírgula).
* A validação `wave_pts == 1024` no firmware evita que o dashboard processe waveforms incompletas.

---

## 📌 O que este Markdown cobre no TCC

* Descrição do pipeline completo (aquisição → nuvem → visualização)
* Justificativa das etapas de DSP (DC removal, janela, rFFT)
* Diagnósticos no firmware para validar qualidade de aquisição (Fs e f_wave)
* Estrutura de dados e formato de envio para integração com Sheets

---
```
