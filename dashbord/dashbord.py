import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import numpy as np
import pandas as pd
import time
from IPython.display import clear_output

# =========================================
# --- 1. CONFIGURAÇÃO ---
# =========================================
URL_ORIGINAL = 'https://docs.google.com/spreadsheets/d/e/2PACX-1vQj7LLJyQ5pLzxLol42OeekPn-CQwAtAgGD5egsZWQtOGtXThBpBB6gf-Vcio0Pn83er6i9pC-sPSrG/pubhtml'
URL_CSV = URL_ORIGINAL.replace('/pubhtml', '/pub?output=csv')

MAX_POINTS = 100
Fs_DEFAULT = 616.2      # use o fs medido no ESP32 (1024 em ~1.66 s)
BANDA_HZ = 5.0          # busca pico em ±5 Hz ao redor da frequência alvo
ZOOM_SECONDS = 0.25     # janela de tempo exibida no gráfico (ex.: 0.25s ~ 15 ciclos)

# =========================================
# --- 2. UTILITÁRIOS DE PARSE ---
# =========================================
def to_float(x):
    """Converte para float lidando com vírgula decimal e strings sujas."""
    try:
        if isinstance(x, str):
            x = x.strip().replace('"', '').replace(',', '.')
        return float(x)
    except:
        return np.nan

def parse_wave_from_row(linha):
    """
    Tenta ler a onda:
    - Caso A: cada amostra em uma coluna (linha[6:], numéricas)
    - Caso B: onda toda em uma coluna como string "a,b,c..."
    """
    # Caso A: colunas 6.. em diante
    vals = pd.to_numeric(linha[6:], errors='coerce')
    vals = vals.dropna()
    if len(vals) > 50:  # suficiente pra dizer que veio em colunas
        wave = vals.values.astype(float).tolist()
        return wave

    # Caso B: tudo em uma string na coluna 6
    if len(linha) > 6 and isinstance(linha[6], str):
        s = linha[6].replace('[', '').replace(']', '').replace('"', '')
        parts = [p.strip() for p in s.split(',') if p.strip() != '']
        wave = []
        for p in parts:
            v = to_float(p)
            if not np.isnan(v):
                wave.append(v)
        if len(wave) > 50:
            return wave

    return []

def processar_pacote_exato(linha):
    try:
        # Coluna 0: tempo
        raw_date = str(linha[0])
        hora = raw_date.split(' ')[-1] if ' ' in raw_date else raw_date

        # Colunas 1..5: escalares
        tensao = to_float(linha[1])
        corrente = to_float(linha[2])
        potencia = to_float(linha[3])
        fp = to_float(linha[4])
        freq = to_float(linha[5])

        # (Opcional futuro) se você mandar FS numa coluna extra, você pode adaptar aqui.
        # Por enquanto, usa default.
        fs = Fs_DEFAULT

        # Onda
        onda = parse_wave_from_row(linha)
        if len(onda) == 0:
            onda = [0.0] * 1024

        # Garante tamanho 1024 (corta ou completa)
        if len(onda) > 1024:
            onda = onda[:1024]
        elif len(onda) < 1024:
            onda = onda + [0.0] * (1024 - len(onda))

        return {
            'timestamp': hora,
            'tensao': tensao,
            'corrente_rms': corrente,
            'potencia': potencia,
            'pf': fp,
            'freq': freq,
            'fs': fs,
            'onda': onda
        }
    except:
        return None

def ler_google_sheets():
    try:
        df = pd.read_csv(URL_CSV, header=None, on_bad_lines='skip')
        df = df.dropna(how='all')
        df = df.dropna(subset=[1, 2])  # garante tensão/corrente existentes

        linhas_para_ler = df.tail(MAX_POINTS)

        historico = {'time': [], 'v': [], 'i': [], 'p': [], 'pf': [], 'f': []}
        ultima_leitura = None

        for i in range(len(linhas_para_ler)):
            linha = linhas_para_ler.iloc[i]
            dados = processar_pacote_exato(linha)
            if dados:
                historico['time'].append(dados['timestamp'])
                historico['v'].append(dados['tensao'])
                historico['i'].append(dados['corrente_rms'])
                historico['p'].append(dados['potencia'])
                historico['pf'].append(dados['pf'])
                historico['f'].append(dados['freq'])
                ultima_leitura = dados

        return historico, ultima_leitura
    except:
        return None, None

# =========================================
# --- 3. DSP (FFT + HARMÔNICAS) ---
# =========================================
def normalize_signal(x):
    x = np.asarray(x, dtype=float)
    m = np.max(np.abs(x))
    return x / m if m and m > 0 else x

def rfft_magnitude(x, fs):
    """
    FFT correta p/ harmônicas:
    - remove DC
    - aplica janela Hann
    - rFFT (só positivos)
    - magnitude aproximada (amplitude pico)
    """
    x = np.asarray(x, dtype=float)
    x = x - np.mean(x)               # DC removal
    w = np.hanning(len(x))
    X = np.fft.rfft(x * w)
    freqs = np.fft.rfftfreq(len(x), d=1/fs)

    # magnitude (amplitude pico aproximada) corrigida pela janela
    mag = (2.0 / np.sum(w)) * np.abs(X)
    return freqs, mag

def get_peak_near(freqs, mag, f0, bw=BANDA_HZ):
    mask = (freqs >= (f0 - bw)) & (freqs <= (f0 + bw))
    if not np.any(mask):
        return 0.0
    return float(np.max(mag[mask]))

# =========================================
# --- 4. DASHBOARD ---
# =========================================
def atualizar_graficos(hist, atual):
    clear_output(wait=True)

    plt.style.use('dark_background')
    cor_fundo = '#1e1e1e'

    fig = plt.figure(figsize=(16, 14), constrained_layout=True)
    fig.patch.set_facecolor(cor_fundo)

    gs = gridspec.GridSpec(3, 4, figure=fig, height_ratios=[1, 2, 2])

    def plot_topo(pos, x, y, tit, cor, fmt="{:.2f}"):
        ax = fig.add_subplot(pos)
        ax.set_facecolor(cor_fundo)
        ax.plot(x, y, color=cor, linewidth=2)
        val = y[-1] if len(y) > 0 and pd.notna(y[-1]) else 0.0
        ax.set_title(f"{tit}: " + fmt.format(val), color=cor, fontweight='bold')
        ax.grid(alpha=0.2)
        ax.set_xticks([])

    plot_topo(gs[0, 0], hist['time'], hist['v'],  "Tensão (V)",   '#FF5252')
    plot_topo(gs[0, 1], hist['time'], hist['i'],  "Corrente (A)", '#448AFF', fmt="{:.3f}")
    plot_topo(gs[0, 2], hist['time'], hist['p'],  "Potência (W)", '#FFAB40')
    plot_topo(gs[0, 3], hist['time'], hist['pf'], "FP",          '#00E676')

    if atual and len(atual['onda']) == 1024:
        user_data = np.array(atual['onda'], dtype=float)

        # Fs (por enquanto default medido)
        fs = float(atual.get('fs', Fs_DEFAULT))
        f_nyq = fs / 2.0
        kmax = int(np.floor((f_nyq) / 60.0))

        # referência 60 Hz (ou usa freq medida se estiver “normal”)
        f0 = atual.get('freq', 60.0)
        if not (40.0 <= f0 <= 70.0):
            f0 = 60.0

        # ----- Forma de onda no tempo -----
        ax1 = fig.add_subplot(gs[1, :])
        ax1.set_facecolor(cor_fundo)

        # normaliza só pra visual
        x_vis = normalize_signal(user_data - np.mean(user_data))
        t = np.arange(len(x_vis)) / fs
        ref = np.sin(2 * np.pi * f0 * t)

        # janela de tempo (ex.: 0.25s)
        n_zoom = int(min(len(x_vis), max(10, ZOOM_SECONDS * fs)))
        ax1.plot(t[:n_zoom], x_vis[:n_zoom], label='Corrente (normalizada, DC removido)', linewidth=2)
        ax1.plot(t[:n_zoom], ref[:n_zoom], label=f'Referência {f0:.1f} Hz', linestyle='--', alpha=0.6)

        ax1.set_title(
            f"Forma de Onda (tempo) | Fs={fs:.1f} SPS | Nyq={f_nyq:.1f} Hz | kmax≈{kmax} | {atual['timestamp']}",
            fontsize=13
        )
        ax1.set_xlabel("Tempo (s)")
        ax1.set_ylabel("Amplitude (norm.)")
        ax1.grid(True, linestyle=':', alpha=0.3)
        ax1.legend(loc='upper right')

        # ----- FFT / Harmônicas -----
        ax2 = fig.add_subplot(gs[2, :])
        ax2.set_facecolor(cor_fundo)

        freqs, mag = rfft_magnitude(user_data, fs)

        # limita ao Nyquist e até ~min(350, Nyquist)
        fmax_plot = min(350.0, f_nyq)
        mask = (freqs >= 0) & (freqs <= fmax_plot)
        freqs_plot = freqs[mask]
        mag_plot = mag[mask]

        ax2.plot(freqs_plot, mag_plot, linewidth=2)
        ax2.fill_between(freqs_plot, mag_plot, alpha=0.2)

        ax2.set_title("Espectro (rFFT + janela Hann + DC removal)", fontsize=13)
        ax2.set_xlabel("Frequência (Hz)")
        ax2.set_ylabel("Magnitude (ampl. pico aprox.)")
        ax2.grid(True, linestyle='--', alpha=0.25)

        # Harmônicas até 5ª (se Nyquist permitir)
        targets = [60, 120, 180, 240, 300]
        peaks = {}
        for ft in targets:
            if ft <= f_nyq:
                peaks[ft] = get_peak_near(freqs, mag, ft, bw=BANDA_HZ)
            else:
                peaks[ft] = 0.0

        # THD até 5ª: sqrt(H2^2+H3^2+H4^2+H5^2) / H1
        H1 = peaks[60]
        H2 = peaks[120]
        H3 = peaks[180]
        H4 = peaks[240]
        H5 = peaks[300]
        thd5 = (np.sqrt(H2**2 + H3**2 + H4**2 + H5**2) / H1 * 100.0) if H1 > 0 else 0.0

        # ticks harmônicas
        xt = [x for x in targets if x <= fmax_plot]
        ax2.set_xticks(xt)
        ax2.set_xticklabels([f"{x}Hz" for x in xt])

        info_text = (
            f"Fs={fs:.1f} SPS | Nyq={f_nyq:.1f} Hz | kmax≈{kmax}\n"
            f"Picos (±{BANDA_HZ:.0f}Hz):\n"
            f"60 (H1):  {H1:.3f}\n"
            f"120(H2):  {H2:.3f}\n"
            f"180(H3):  {H3:.3f}\n"
            f"240(H4):  {H4:.3f}\n"
            f"300(H5):  {H5:.3f}\n"
            f"THD₅(2..5): {thd5:.2f}%"
        )

        ax2.text(
            0.98, 0.95, info_text,
            transform=ax2.transAxes,
            fontsize=11,
            bbox=dict(facecolor='black', alpha=0.75, edgecolor='white'),
            ha='right', va='top'
        )

    plt.show()

# =========================================
# --- 5. LOOP ---
# =========================================
print("Iniciando Monitoramento (FFT corrigida + THD₅)...")
try:
    while True:
        hist, atual = ler_google_sheets()
        if atual:
            atualizar_graficos(hist, atual)
            print(f"Atualizado em: {atual['timestamp']} | Wave: {len(atual['onda'])} pts | Fs usado: {atual.get('fs', Fs_DEFAULT)}")
        else:
            print("Aguardando dados...")
        time.sleep(3)
except KeyboardInterrupt:
    print("Parado.")

