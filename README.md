# 🎮 Mesa Labirinto Controlada por Joystick

> Projeto Final — Sistemas Embarcados 2026.1  
> Universidade Estadual da Paraíba

---

## 📋 Descrição

Sistema embarcado baseado no **ESP32-S3** que controla uma mesa com labirinto por meio de dois servomotores, permitindo ao usuário guiar uma esfera metálica até a saída usando um joystick analógico.

O sensor inercial **MPU6050**, fixado na mesa, captura os ângulos de inclinação (pitch e roll) em tempo real. Esses dados são transmitidos via serial para um computador, onde um script Python os armazena no banco de dados **InfluxDB 3 Core** e o **Grafana** exibe um dashboard interativo — o **gêmeo digital** da mesa.

---

## 🎥 Vídeo Demonstrativo

> 🔗 **[(https://youtube.com/shorts/jNQh5DvsLao?feature=share)]**

---

## 🧰 Hardware Utilizado

| Quantidade | Componente | Função |
|---|---|---|
| 1 | ESP32-S3-DevKitC-1 | Microcontrolador principal |
| 1 | Joystick analógico (com botão) | Controle dos servos + cronômetro |
| 2 | Servo motor SG90 | Movimento da mesa nos eixos X e Y |
| 1 | MPU6050 | Sensor inercial (pitch e roll) |
| 1 | LED + resistor 330Ω | Indicador de inicialização concluída |
| 1 | Fonte 5V externa (mín. 1A) | Alimentação dos servomotores |

---

## 🔌 Esquema de Ligações

| Componente | Pino | GPIO ESP32-S3 | Observação |
|---|---|---|---|
| Joystick | VRX (eixo X) | GPIO4 | ADC1 Canal 3 |
| Joystick | VRY (eixo Y) | GPIO5 | ADC1 Canal 4 |
| Joystick | SW (botão) | GPIO6 | Pull-up interno — cronômetro |
| Joystick | VCC | 3.3V | Alimentação |
| Joystick | GND | GND | Terra |
| Servo 1 (eixo X) | Sinal | GPIO42 | LEDC, 50 Hz |
| Servo 1 (eixo X) | VCC | 5V externo | Não usar 3.3V do ESP32 |
| Servo 2 (eixo Y) | Sinal | GPIO41 | LEDC, 50 Hz |
| Servo 2 (eixo Y) | VCC | 5V externo | GND comum com ESP32 |
| MPU6050 | SDA | GPIO17 | I2C |
| MPU6050 | SCL | GPIO18 | I2C |
| MPU6050 | AD0 | GND | Endereço I2C: 0x68 |
| MPU6050 | VCC | 3.3V | Alimentação |
| LED de status | Anodo (+) | GPIO48 | Via resistor 330Ω |

> ⚠️ O GND da fonte 5V externa **deve** ser ligado ao GND do ESP32, senão o PWM não controla os servos corretamente.

---

## 🏗️ Arquitetura do Firmware

O código é organizado em **4 tarefas FreeRTOS**, sincronizadas por mutex:

| Tarefa | Prioridade | Frequência | Responsabilidade |
|---|---|---|---|
| `Task_Joystick` | 5 | 50 Hz | Leitura ADC dos eixos X/Y e detecção do botão (cronômetro) |
| `Task_Servos` | 5 | 50 Hz | Conversão ADC → ângulo → PWM, com zona morta e suavização |
| `Task_MPU6050` | 5 | 100 Hz | Leitura I2C, filtro complementar (α=0.98), média móvel 8 amostras |
| `Task_Console` | 3 | ~3 Hz | Debug serial + envio JSON para InfluxDB |

---

## ⚙️ Parâmetros Ajustáveis

Localizados no topo de `main/main.c`:

```c
#define DEADZONE        120   // zona morta joystick — aumentar se vibrar
#define SERVO_MIN       65    // ângulo mínimo do servo (graus)
#define SERVO_MAX       115   // ângulo máximo do servo (graus)
#define SERVO_MID       90    // posição central (mesa nivelada)
#define SERVO_MAX_STEP  3     // suavização — diminuir se for rápido demais
#define SERVO1_OFFSET   0     // offset mecânico do horn — Servo 1
#define SERVO2_OFFSET   -9    // offset mecânico do horn — Servo 2
```

---

## ✨ Diferenciais Implementados

Além dos requisitos obrigatórios das três fases:

1. **Calibração automática do joystick** — lê 50 amostras em repouso e calcula o centro real de cada eixo, compensando desvio mecânico do potenciômetro
2. **Calibração de bias do giroscópio** — 500 amostras com sensor parado eliminam o drift do MPU6050
3. **Calibração de nivelamento da mesa** — após 100 iterações de convergência do filtro, zera automaticamente o pitch/roll com a mesa nivelada
4. **Zona morta + suavização dos servos** — elimina vibração em repouso e movimentos bruscos que jogavam a esfera para fora do labirinto
5. **Offset mecânico do horn** — corrige em software o desalinhamento físico do braço do servo
6. **Cronômetro de partida** — botão do joystick inicia/para o timer; o resultado aparece no terminal e no Grafana em tempo real

---

## 📊 Gêmeo Digital — Grafana + InfluxDB

### Formato JSON enviado pela serial

```
DATA:{"pitch":1.23,"roll":-4.56,"joy_x":2048,"joy_y":2048,
      "servo1":90,"servo2":90,"tempo_partida":5.43,"jogo_ativo":1}
```

### Painéis do dashboard "Mesa Labirinto - Gêmeo Digital"

| Painel | Tipo | Descrição |
|---|---|---|
| Pitch e Roll - Tempo Real | Time series | Gráfico de linha com evolução temporal dos ângulos |
| Orientação da Mesa | Gauge | Valor instantâneo de pitch e roll (escala -45° a 45°) |
| Cronômetro - Última Partida | Stat | Duração da partida em segundos |
| Status (Jogando/Parado) | Stat | Exibe "Jogando..." ou "Parado" conforme `jogo_ativo` |

---

## 🖥️ Requisitos de Software (Computador)

- Python 3.x
- InfluxDB 3 Core
- Grafana OSS

### Instalação das dependências Python

```bash
pip install pyserial influxdb3-python
```

---

## 🚀 Como Executar

### 1. Firmware (ESP32)

```bash
# No VS Code com extensão ESP-IDF
# Abrir pasta: C:\Users\gneto\projeto1
# Build + Flash + Monitor com os botões da barra inferior
```

### 2. InfluxDB

```powershell
cd C:\InfluxDB
.\influxdb3.exe serve --node-id local01
```

Ou duplo clique em `iniciar_influxdb.bat` na área de trabalho.

### 3. Grafana

Acesse `http://localhost:3000` — o Grafana roda automaticamente como serviço Windows.

### 4. Script Python (com ESP32 conectado)

```powershell
# Feche o idf monitor primeiro (Ctrl+])
# Verifique a porta COM:
[System.IO.Ports.SerialPort]::GetPortNames()

# Rode o script:
cd C:\Users\gneto\projeto1
python computador\serial_para_influxdb.py
```

### 5. Script de teste (sem ESP32)

```powershell
python computador\teste_sessao_completa.py
```

Simula uma sessão de ~90 segundos com movimentos contínuos e duas partidas com cronômetro — útil para validar o dashboard antes do teste com hardware.

---

## 📁 Estrutura do Repositório

```
projeto1/
├── main/
│   └── main.c                      # Firmware completo (Fases 1, 2 e 3)
├── computador/
│   ├── serial_para_influxdb.py     # Lê serial do ESP32 e grava no InfluxDB
│   ├── teste_influxdb.py           # Dados simulados simples (10 pontos)
│   ├── teste_sessao_completa.py    # Sessão realista com 2 partidas
│   └── requirements.txt            # Dependências Python
├── CMakeLists.txt
├── iniciar_influxdb.bat            # Atalho para iniciar o InfluxDB
├── .gitignore
└── README.md
```

---

## 🔧 Tecnologias e Bibliotecas

| Biblioteca | Plataforma | Uso |
|---|---|---|
| `esp_adc / adc_oneshot` | C / ESP-IDF v6 | Leitura analógica do joystick |
| `driver/ledc` | C / ESP-IDF v6 | Geração de PWM para os servos |
| `driver/i2c_master` | C / ESP-IDF v6 | Comunicação I2C com MPU6050 (nova API v6) |
| `driver/gpio` | C / ESP-IDF v6 | Controle do LED e botão |
| `esp_timer` | C / ESP-IDF v6 | Cronômetro de alta resolução |
| `freertos/semphr` | FreeRTOS | Mutex para sincronização entre tarefas |
| `math.h` | C padrão | Cálculo de pitch e roll (atan2f, sqrtf) |
| `pyserial` | Python 3 | Leitura da porta serial |
| `influxdb3-python` | Python 3 | Escrita de pontos no InfluxDB 3 Core |
| InfluxDB 3 Core | Banco de dados | Séries temporais (pitch, roll, servos) |
| Grafana OSS | Dashboard | Visualização em tempo real |

---

## 👥 Equipe

- Geraldo Silveira Neto
- Alexandre Freitas de Lima Pacheco
- Cefras José Ferreira Mandú de Almeida
- Geraldo Silveira Neto
- Raul Confessor Oliveira Silva
- Robson Luan Pereira Fernandes


---

## 📄 Licença

Projeto acadêmico — Universidade Estadual da Paraíba — 2026.1
