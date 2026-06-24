"""
Le os dados JSON enviados pelo ESP32 via serial e grava no InfluxDB 3 Core.

Antes de rodar, instale as bibliotecas:
    pip install pyserial influxdb3-python
"""

import serial
import json
from influxdb_client_3 import InfluxDBClient3, Point

# ================= CONFIGURACAO =================
SERIAL_PORT     = "COM4"          # AJUSTE: porta onde o ESP32 esta conectado
BAUD_RATE       = 115200

INFLUX_HOST     = "http://localhost:8181"
INFLUX_TOKEN    = "SEU_TOKEN"
INFLUX_DATABASE = "mesa_labirinto"
# ==================================================


def main():
    print(f"Conectando ao InfluxDB 3 em {INFLUX_HOST} ...")
    client = InfluxDBClient3(
        host=INFLUX_HOST,
        token=INFLUX_TOKEN,
        database=INFLUX_DATABASE,
    )

    print(f"Abrindo porta serial {SERIAL_PORT} @ {BAUD_RATE} ...")
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)

    print("Lendo dados do ESP32. Pressione Ctrl+C para parar.\n")

    while True:
        try:
            linha = ser.readline().decode("utf-8", errors="ignore").strip()

            if not linha.startswith("DATA:"):
                continue

            json_texto = linha[len("DATA:"):]
            dados = json.loads(json_texto)

            ponto = (
                Point("mesa_labirinto")
                .field("pitch", float(dados["pitch"]))
                .field("roll", float(dados["roll"]))
                .field("joy_x", int(dados["joy_x"]))
                .field("joy_y", int(dados["joy_y"]))
                .field("servo1", int(dados["servo1"]))
                .field("servo2", int(dados["servo2"]))
                .field("tempo_partida", float(dados.get("tempo_partida", 0.0)))
                .field("jogo_ativo", int(dados.get("jogo_ativo", 0)))
            )

            client.write(ponto)

            status_jogo = "JOGANDO" if dados.get("jogo_ativo", 0) == 1 else "parado"
            print(f"Pitch: {dados['pitch']:+6.1f}  |  Roll: {dados['roll']:+6.1f}  "
                  f"|  Servo1: {dados['servo1']:3d}  Servo2: {dados['servo2']:3d}  "
                  f"|  Tempo: {dados.get('tempo_partida', 0.0):6.2f}s [{status_jogo}]")

        except json.JSONDecodeError:
            continue
        except KeyError as e:
            print(f"Campo faltando no JSON: {e}")
        except KeyboardInterrupt:
            print("\nEncerrando...")
            break
        except Exception as e:
            print(f"Erro: {e}")

    ser.close()
    client.close()


if __name__ == "__main__":
    main()
