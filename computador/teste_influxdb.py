"""
Escreve alguns pontos de teste no InfluxDB 3, sem precisar do ESP32.

Isso cria a tabela "mesa_labirinto" com os mesmos campos que o
script real (serial_para_influxdb.py) vai usar, permitindo montar
e testar o dashboard no Grafana antes de ter o ESP32 em maos.

Requisitos:
    pip install influxdb3-python
"""

import time
from influxdb_client_3 import InfluxDBClient3, Point

# ================= CONFIGURACAO =================
INFLUX_HOST     = "http://localhost:8181"
INFLUX_TOKEN    = "apiv3_Vdno-gC3EJvlR0VwzJ1Zr7iB-STK4be7Ce3O5BeQ-_wJH3hnWJi8vnYxl1Agz8r_BElw9jELwJKw8uCxxrxy3g"
INFLUX_DATABASE = "mesa_labirinto"
# ==================================================


def main():
    print(f"Conectando ao InfluxDB 3 em {INFLUX_HOST} ...")
    client = InfluxDBClient3(
        host=INFLUX_HOST,
        token=INFLUX_TOKEN,
        database=INFLUX_DATABASE,
    )

    print("Enviando 10 pontos de teste (1 por segundo)...\n")

    for i in range(10):
        # Simula a mesa inclinando de -5 a +4 graus
        pitch = float(i - 5)
        roll  = float((i - 5) * 2)

        ponto = (
            Point("mesa_labirinto")
            .field("pitch", pitch)
            .field("roll", roll)
            .field("joy_x", 2048)
            .field("joy_y", 2048)
            .field("servo1", 90)
            .field("servo2", 90)
        )

        client.write(ponto)
        print(f"  [{i+1}/10] pitch={pitch:+.1f}  roll={roll:+.1f}  -> enviado")
        time.sleep(1)

    client.close()
    print("\nConcluido! A tabela 'mesa_labirinto' foi criada no InfluxDB.")
    print("Agora abra o Grafana e monte o dashboard - os dados de teste vao aparecer.")


if __name__ == "__main__":
    main()
