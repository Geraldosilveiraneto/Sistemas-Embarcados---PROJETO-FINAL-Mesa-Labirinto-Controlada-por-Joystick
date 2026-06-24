"""
Simula uma sessao de jogo realista e longa na Mesa Labirinto,
para gerar dados suficientes no Grafana para tirar prints.

Simula:
- Movimentos continuos e suaves do joystick (como um jogador real)
- Pitch e roll variando organicamente (mesa inclinando)
- 3 partidas completas com cronometro (inicio, jogo, fim)
- Duracao total: ~90 segundos

Requisitos:
    pip install influxdb3-python
"""

import time
import math
import random
from influxdb_client_3 import InfluxDBClient3, Point

# ================= CONFIGURACAO =================
INFLUX_HOST     = "http://localhost:8181"
INFLUX_TOKEN    = "SEU_TOKEN"
INFLUX_DATABASE = "mesa_labirinto"
# ==================================================

INTERVALO_S = 0.3   # mesmo intervalo do ESP32 real (300ms)


def enviar_ponto(client, pitch, roll, joy_x, joy_y, servo1, servo2, tempo_partida, jogo_ativo):
    # float(...) garante que pitch/roll/tempo_partida sempre vao como
    # float no line protocol, mesmo quando o valor e um numero "redondo"
    # como 0 -- caso contrario o InfluxDB pode inferir "integer" e dar
    # conflito de tipo com a coluna ja existente na tabela.
    ponto = (
        Point("mesa_labirinto")
        .field("pitch", float(round(pitch, 2)))
        .field("roll", float(round(roll, 2)))
        .field("joy_x", int(joy_x))
        .field("joy_y", int(joy_y))
        .field("servo1", int(servo1))
        .field("servo2", int(servo2))
        .field("tempo_partida", float(round(tempo_partida, 2)))
        .field("jogo_ativo", int(jogo_ativo))
    )
    client.write(ponto)


def simular_movimento(t, amplitude=15, velocidade=1.0, ruido=2.0):
    """Gera um movimento senoidal suave com um pouco de ruido, como um jogador real."""
    base = amplitude * math.sin(t * velocidade)
    return base + random.uniform(-ruido, ruido)


def main():
    print(f"Conectando ao InfluxDB 3 em {INFLUX_HOST} ...")
    client = InfluxDBClient3(host=INFLUX_HOST, token=INFLUX_TOKEN, database=INFLUX_DATABASE)

    print("Iniciando sessao de jogo simulada (~90 segundos)...")
    print("Abra o Grafana agora para acompanhar em tempo real!\n")

    t_global = 0.0

    # ===================================================
    # FASE 1 - Sistema parado, aguardando jogador (5s)
    # ===================================================
    print("[FASE 1] Sistema parado, aguardando jogador...")
    for _ in range(int(5 / INTERVALO_S)):
        enviar_ponto(client, 0.0, 0.0, 2048, 2048, 90, 90, 0.0, 0)
        time.sleep(INTERVALO_S)
        t_global += INTERVALO_S

    # ===================================================
    # FASE 2 - Partida 1: jogador comecando, movimentos leves (20s)
    # ===================================================
    print("\n[FASE 2] *** PARTIDA 1 INICIADA *** (movimentos leves)")
    inicio_partida = time.time()
    for i in range(int(20 / INTERVALO_S)):
        t = i * INTERVALO_S
        pitch = simular_movimento(t, amplitude=8, velocidade=0.8, ruido=1.0)
        roll  = simular_movimento(t + 1.5, amplitude=8, velocidade=0.6, ruido=1.0)

        joy_x = int(2048 + pitch * 30)
        joy_y = int(2048 + roll * 30)
        servo1 = int(90 + pitch * 1.5)
        servo2 = int(90 + roll * 1.5)
        tempo_decorrido = time.time() - inicio_partida

        enviar_ponto(client, pitch, roll, joy_x, joy_y, servo1, servo2, tempo_decorrido, 1)
        if i % 5 == 0:
            print(f"  t={tempo_decorrido:5.1f}s  pitch={pitch:+5.1f}  roll={roll:+5.1f}")
        time.sleep(INTERVALO_S)

    tempo_final_1 = time.time() - inicio_partida
    print(f"  *** TEMPO FINAL PARTIDA 1: {tempo_final_1:.2f}s ***")
    for _ in range(int(2 / INTERVALO_S)):
        enviar_ponto(client, 0.0, 0.0, 2048, 2048, 90, 90, tempo_final_1, 0)
        time.sleep(INTERVALO_S)

    # ===================================================
    # FASE 3 - Pausa entre partidas (5s)
    # ===================================================
    print("\n[FASE 3] Pausa entre partidas...")
    for _ in range(int(5 / INTERVALO_S)):
        enviar_ponto(client, 0.0, 0.0, 2048, 2048, 90, 90, tempo_final_1, 0)
        time.sleep(INTERVALO_S)

    # ===================================================
    # FASE 4 - Partida 2: jogador mais agressivo (25s)
    # ===================================================
    print("\n[FASE 4] *** PARTIDA 2 INICIADA *** (movimentos mais intensos)")
    inicio_partida = time.time()
    for i in range(int(25 / INTERVALO_S)):
        t = i * INTERVALO_S
        pitch = simular_movimento(t, amplitude=20, velocidade=1.4, ruido=3.0)
        roll  = simular_movimento(t + 2.0, amplitude=20, velocidade=1.1, ruido=3.0)

        joy_x = int(2048 + pitch * 30)
        joy_x = max(0, min(4095, joy_x))
        joy_y = int(2048 + roll * 30)
        joy_y = max(0, min(4095, joy_y))
        servo1 = int(90 + pitch * 1.5)
        servo1 = max(65, min(115, servo1))
        servo2 = int(90 + roll * 1.5)
        servo2 = max(65, min(115, servo2))
        tempo_decorrido = time.time() - inicio_partida

        enviar_ponto(client, pitch, roll, joy_x, joy_y, servo1, servo2, tempo_decorrido, 1)
        if i % 5 == 0:
            print(f"  t={tempo_decorrido:5.1f}s  pitch={pitch:+5.1f}  roll={roll:+5.1f}")
        time.sleep(INTERVALO_S)

    tempo_final_2 = time.time() - inicio_partida
    print(f"  *** TEMPO FINAL PARTIDA 2: {tempo_final_2:.2f}s ***")
    for _ in range(int(2 / INTERVALO_S)):
        enviar_ponto(client, 0.0, 0.0, 2048, 2048, 90, 90, tempo_final_2, 0)
        time.sleep(INTERVALO_S)

    # ===================================================
    # FASE 5 - Pausa final (5s)
    # ===================================================
    print("\n[FASE 5] Sessao finalizada, sistema em repouso...")
    for _ in range(int(5 / INTERVALO_S)):
        enviar_ponto(client, 0.0, 0.0, 2048, 2048, 90, 90, tempo_final_2, 0)
        time.sleep(INTERVALO_S)

    client.close()
    print("\n" + "="*50)
    print("SESSAO COMPLETA!")
    print(f"  Partida 1: {tempo_final_1:.2f}s")
    print(f"  Partida 2: {tempo_final_2:.2f}s")
    print("Va ate o Grafana e tire os prints agora.")
    print("Dica: mude o intervalo para 'Last 5 minutes' para ver tudo.")
    print("="*50)


if __name__ == "__main__":
    main()
