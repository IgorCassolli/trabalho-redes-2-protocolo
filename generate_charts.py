import os
import re
import matplotlib.pyplot as plt

CAPTURE_TIME_SECONDS = 30.0

def extract_delay(filename):
    times = []
    if os.path.exists(filename):
        with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                m = re.search(r'time=([\d\.]+)', line)
                if m:
                    times.append(float(m.group(1)))
    return times if times else [0]

def extract_table_size(filename):
    count = 0
    if os.path.exists(filename):
        with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                if re.match(r'^\s*(R|O|C|K|S)\b', line) or '10.0.' in line:
                    count += 1
    return count if count > 0 else 0

def extract_packets_and_rate(filename):
    packet_count = 0
    total_bytes = 0
    if os.path.exists(filename):
        with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                m = re.search(r'length\s+(\d+)', line)
                if m:
                    packet_count += 1
                    total_bytes += int(m.group(1))
    rate_bps = (total_bytes * 8) / CAPTURE_TIME_SECONDS if CAPTURE_TIME_SECONDS > 0 else 0
    return packet_count, rate_bps

protocols = ['RIPv2', 'OSPFv2', 'Mini-RIP (C)']

# Data Processing
delays = [
    sum(extract_delay('metricas/ping_rip.txt')) / max(1, len(extract_delay('metricas/ping_rip.txt'))),
    sum(extract_delay('metricas/ping_ospf.txt')) / max(1, len(extract_delay('metricas/ping_ospf.txt'))),
    sum(extract_delay('metricas/ping_proprio.txt')) / max(1, len(extract_delay('metricas/ping_proprio.txt')))
]

tables = [
    extract_table_size('metricas/rotas_rip.txt'),
    extract_table_size('metricas/rotas_ospf.txt'),
    extract_table_size('metricas/rotas_proprio.txt')
]

pkt_rip, rate_rip = extract_packets_and_rate('metricas/pacotes_rip.txt')
pkt_ospf, rate_ospf = extract_packets_and_rate('metricas/pacotes_ospf.txt')
pkt_prop, rate_prop = extract_packets_and_rate('metricas/pacotes_proprio.txt')

packets = [pkt_rip, pkt_ospf, pkt_prop]
rates_bps = [rate_rip, rate_ospf, rate_prop]

# Chart Generation
fig, axs = plt.subplots(2, 2, figsize=(12, 9))
colors = ['#3b82f6', '#10b981', '#f59e0b']

axs[0, 0].bar(protocols, tables, color=colors)
axs[0, 0].set_title('1. Tamanho da Tabela de Roteamento')
axs[0, 0].set_ylabel('Nº de Entradas')

axs[0, 1].bar(protocols, packets, color=colors)
axs[0, 1].set_title('2. Pacotes de Roteamento Enviados (30s)')
axs[0, 1].set_ylabel('Quantidade de Pacotes')

axs[1, 0].bar(protocols, rates_bps, color=colors)
axs[1, 0].set_title('3. Taxa de Transmissão do Protocolo')
axs[1, 0].set_ylabel('Taxa (bps)')

axs[1, 1].bar(protocols, delays, color=colors)
axs[1, 1].set_title('4. Delay Médio Fim-a-Fim R1 -> R5')
axs[1, 1].set_ylabel('RTT Médio (ms)')

for ax in axs.flat:
    ax.grid(axis='y', linestyle='--', alpha=0.6)
    for p in ax.patches:
        ax.annotate(f'{p.get_height():.2f}', (p.get_x() + p.get_width() / 2., p.get_height()), ha='center', va='bottom')

plt.tight_layout()
plt.savefig('metricas_comparativas.png', dpi=150)
print("Gráfico gerado: metricas_comparativas.png")