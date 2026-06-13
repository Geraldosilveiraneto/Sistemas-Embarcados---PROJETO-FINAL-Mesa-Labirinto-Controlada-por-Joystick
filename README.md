# Mesa Labirinto Controlada por Joystick

Projeto final da disciplina de Sistemas Embarcados (2026.1). Sistema embarcado
baseado no ESP32-S3 que controla uma mesa com labirinto através de dois
servomotores, movidos por um joystick analógico. Um sensor inercial MPU6050
mede a inclinação da mesa (pitch/roll) e os dados são transmitidos via serial
para um "gêmeo digital" visualizado em tempo real no Grafana.

## Sumário

- [Visão geral](#visão-geral)
- [Hardware](#hardware)
- [Arquitetura de software](#arquitetura-de-software)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Como compilar e gravar (ESP-IDF)](#como-compilar-e-gravar-esp-idf)
- [Gêmeo digital: InfluxDB + Grafana](#gêmeo-digital-influxdb--grafana)
- [Status do projeto](#status-do-projeto)
- [Dificuldades e soluções](#dificuldades-e-soluções)
- [Vídeo demonstrativo](#vídeo-demonstrativo)
- [Equipe](#equipe)

## Visão geral

O usuário movimenta um joystick analógico, que controla dois servomotores
responsáveis por inclinar a mesa nos eixos X e Y. Uma esfera de aço percorre
o labirinto até atingir o furo de saída. Um MPU6050 fixado na mesa mede os
ângulos reais de inclinação e envia esses dados ao computador, onde são
armazenados no InfluxDB e exibidos em tempo real em um dashboard Grafana que
replica visualmente o comportamento físico da mesa.

## Hardware

| Componente            | Quantidade | Função                                   |
|-----------------------|-----------|-------------------------------------------|
| ESP32-S3              | 1         | Microcontrolador principal                 |
| Joystick analógico    | 1         | Controle dos servos nos eixos X e Y        |
| Servo motor (SG90)    | 2         | Movimento da mesa nos eixos X e Y          |
| MPU6050               | 1         | Sensor inercial (pitch e roll)             |
| LED                   | 1         | Indica fim da inicialização do sistema     |

### Mapeamento de pinos (ESP32-S3)

| Sinal                  | GPIO    | Observação                                  |
|------------------------|---------|----------------------------------------------|
| Joystick eixo X (VRX)  | GPIO4   | ADC1 canal 3                                  |
| Joystick eixo Y (VRY)  | GPIO5   | ADC1 canal 4                                  |
| Servo 1 (eixo X) PWM   | GPIO42  | LEDC, 50 Hz                                   |
| Servo 2 (eixo Y) PWM   | GPIO41  | LEDC, 50 Hz                                   |
| MPU6050 SDA            | GPIO17  | I2C                                            |
| MPU6050 SCL            | GPIO18  | I2C                                            |
| MPU6050 AD0            | GND     | Define endereço I2C como 0x68                 |
| LED de status          | GPIO48  | Acende ao final da inicialização              |

> Observação: GPIO42 não pôde ser usado para o I2C porque, no ESP32-S3,
> corresponde ao pino MTMS (JTAG/depuração).

### Alimentação

- ESP32-S3, joystick e MPU6050: 3.3V (regulador interno da placa)
- Servos: 5V externo, com GND comum ao ESP32

## Arquitetura de software

O firmware é organizado em 4 tarefas FreeRTOS, sincronizadas por um mutex
que protege as estruturas de dados compartilhadas:

| Tarefa            | Responsabilidade                                              | Frequência |
|-------------------|----------------------------------------------------------------|------------|
| `Task_Joystick`   | Leitura analógica dos eixos X e Y via ADC one-shot              | 50 Hz      |
| `Task_Servos`     | Converte a leitura do joystick em ângulo (0-180°) e PWM (LEDC)  | 50 Hz      |
| `Task_MPU6050`    | Lê acelerômetro e giroscópio via I2C, calcula pitch/roll com filtro complementar (98% giroscópio / 2% acelerômetro) + DLPF interno (44 Hz) + média móvel de 8 amostras | 100 Hz |
| `Task_Console`    | Imprime status de debug e envia uma linha `DATA:{json}` com pitch, roll, joystick e ângulos dos servos | ~3 Hz |

O LED de status é aceso após a inicialização de todos os periféricos
(ADC, servos, MPU6050), sinalizando que o sistema está pronto para uso.

## Estrutura do repositório

```
.
├── CMakeLists.txt
├── sdkconfig
├── main/
│   ├── CMakeLists.txt
│   └── main.c                  # Firmware ESP32-S3 (Fases 1-3)
├── computador/
│   ├── requirements.txt        # Dependencias Python
│   ├── serial_para_influxdb.py # Le a serial e grava no InfluxDB
│   └── teste_influxdb.py       # Gera dados de teste (sem ESP32)
└── README.md
```

## Como compilar e gravar (ESP-IDF)

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Substitua `COMx` pela porta serial onde o ESP32-S3 está conectado.

## Gêmeo digital: InfluxDB + Grafana

### 1. InfluxDB 3 Core

```powershell
.\influxdb3.exe serve --node-id=local01 --object-store=file --data-dir=.\data
.\influxdb3.exe create token --admin
.\influxdb3.exe create database mesa_labirinto --token apiv3_SEU_TOKEN
```

### 2. Script Python

```bash
pip install -r computador/requirements.txt
```

Edite `computador/serial_para_influxdb.py` e preencha `SERIAL_PORT` e
`INFLUX_TOKEN` com os seus valores. Em seguida:

```bash
python computador/serial_para_influxdb.py
```

O script lê linhas `DATA:{...}` enviadas pelo ESP32 e grava cada ponto
(pitch, roll, posição do joystick, ângulo dos servos) na tabela
`mesa_labirinto` do InfluxDB.

### 3. Grafana

Fonte de dados InfluxDB (Query Language: SQL, URL `http://localhost:8181`,
Insecure Connection ativado, Database `mesa_labirinto`, token do passo 1).

Query usada nos painéis:

```sql
SELECT time, pitch, roll FROM mesa_labirinto WHERE $__timeFilter(time) ORDER BY time
```

Dashboard "Mesa Labirinto - Gêmeo Digital" com dois painéis:
- **Pitch e Roll - Tempo Real** (Time series)
- **Orientação da Mesa (Pitch/Roll)** (Gauge, escala -45° a 45°)

## Status do projeto

- [x] Fase 1 — Controle local da mesa (joystick + servos + FreeRTOS)
- [x] Fase 2 — Leitura de orientação (MPU6050 + filtro complementar)
- [x] Fase 3 — Software do gêmeo digital (InfluxDB + Grafana) pronto;
      integração final com o hardware em andamento

## Dificuldades e soluções

- **GPIO42 indisponível para I2C no ESP32-S3**: esse pino corresponde ao
  MTMS (JTAG). O I2C do MPU6050 foi movido para GPIO17 (SDA) e GPIO18 (SCL).
- **Variáveis não inicializadas (`-Werror=maybe-uninitialized`)**: o
  ESP-IDF v6 trata warnings como erros; resolvido inicializando todas as
  variáveis locais lidas pelo mutex.
- **Leitura instável do MPU6050 (oscilação mesmo com a mesa parada)**:
  resolvido combinando três técnicas: filtro passa-baixa interno do sensor
  (DLPF a 44 Hz), filtro complementar com peso de 98% para o giroscópio, e
  média móvel de 8 amostras sobre o resultado final.
- **API I2C legada (`i2c_driver_install`) incompatível com ESP-IDF v6**:
  migrado para a nova API `driver/i2c_master.h` (`i2c_new_master_bus`,
  `i2c_master_transmit_receive`).
- **InfluxDB**: o projeto usa o **InfluxDB 3 Core** (não a v2), que não
  possui interface web; toda a configuração (token, banco de dados) é feita
  via CLI (`influxdb3 create token --admin`, `influxdb3 create database`).

## Vídeo demonstrativo

[Link do YouTube aqui]

## Equipe

- Nome do integrante 1
- Nome do integrante 2
- Nome do integrante 3
