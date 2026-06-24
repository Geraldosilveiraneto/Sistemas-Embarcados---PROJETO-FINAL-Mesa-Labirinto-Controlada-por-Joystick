#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_timer.h"

/* =========================================================
   PINAGEM Fase3 esp32
   ========================================================= */

#define JOY_X_CHANNEL   ADC_CHANNEL_3    // GPIO4
#define JOY_Y_CHANNEL   ADC_CHANNEL_4    // GPIO5
#define JOY_BTN_PIN     GPIO_NUM_6
#define ADC_UNIT_USED   ADC_UNIT_1

#define SERVO1_PIN      GPIO_NUM_42
#define SERVO2_PIN      GPIO_NUM_41
#define PWM_FREQ_HZ     50

#define LED_PIN         GPIO_NUM_48

#define I2C_SDA_PIN     GPIO_NUM_17
#define I2C_SCL_PIN     GPIO_NUM_18
#define ACCEL_SENS      16384.0f
#define GYRO_SENS       131.0f

#define FILTRO_ALPHA    0.98f
#define MEDIA_AMOSTRAS  8
#define CALIB_AMOSTRAS  500

/* Quantas amostras de pitch/roll usar para calibrar o "zero" do
   MPU6050 no boot. Mesa deve estar parada e nivelada nesse momento. */
#define CALIB_NIVEL_AMOSTRAS  100

/* =========================================================
   AJUSTES DE COMPORTAMENTO DOS SERVOS
   Mexa aqui se precisar ajustar apos testar no hardware.
   ========================================================= */

/* Zona morta: ignora oscilacoes pequenas do joystick no centro.
   Se o servo ainda vibrar parado, aumente esse valor (ex: 150). */
#define DEADZONE        120

/* Limite de angulo: restringe o quanto a mesa pode inclinar.
   60-120 = inclinacao suave. 45-135 = mais agressivo. */
#define SERVO_MIN       65
#define SERVO_MAX       115
#define SERVO_MID       90

/* Suavizacao do servo: limita o quanto o angulo muda por ciclo (20ms).
   1 = movimento muito lento. 5 = rapido mas suave. 10 = sem filtro.
   Se a mesa ainda for rapida demais, diminua (ex: 2). */
#define SERVO_MAX_STEP  3

/* =========================================================
   OFFSET DE CALIBRACAO MECANICA
   Compensa o horn do servo desalinhado fisicamente no eixo.
   O servo recebe pulso eletrico de X graus, mas o braço fica
   fisicamente torto porque o horn nao foi encaixado no centro
   exato do eixo serrilhado. O offset corrige isso em software.

   Como descobrir o valor: use o teste manual (digitar angulo
   no serial), mande o servo para 90 graus, veja em qual angulo
   ele FICA FISICAMENTE RETO, e calcule:
       offset = angulo_reto - 90

   Servo2 (GPIO41): fica reto em 81 graus -> offset = 81 - 90 = -9
   ========================================================= */
#define SERVO1_OFFSET   0     // Servo 1 (GPIO42, eixo X) - sem desvio detectado
#define SERVO2_OFFSET   -9    // Servo 2 (GPIO41, eixo Y) - corrige horn torto

/* =========================================================
   VARIAVEIS COMPARTILHADAS
   ========================================================= */

static SemaphoreHandle_t mutex              = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static i2c_master_bus_handle_t   i2c_bus;
static i2c_master_dev_handle_t   mpu_dev;
static bool mpu_ok = false;

static float gyro_bias_x = 0.0f;
static float gyro_bias_y = 0.0f;

/* Bias de nivelamento: zera pitch/roll com a mesa nivelada no boot.
   Calculado automaticamente em calibrar_nivel_mesa(). */
static float pitch_bias = 0.0f;
static float roll_bias  = 0.0f;

static int joy_center_x = 2048;
static int joy_center_y = 2048;

static int joy_x = 2048;
static int joy_y = 2048;

typedef struct {
    float pitch;
    float roll;
} mpu_data_t;

static mpu_data_t mpu_data = {0.0f, 0.0f};

static bool     jogo_ativo     = false;
static int64_t  jogo_inicio_us = 0;
static float    ultimo_tempo_s = 0.0f;

/* =========================================================
   FUNCOES AUXILIARES — SERVO
   ========================================================= */

/**
 * Converte ADC para angulo com zona morta e range limitado.
 */
static int adc_para_angulo(int adc_val, int centro)
{
    if (adc_val < 0)    adc_val = 0;
    if (adc_val > 4095) adc_val = 4095;

    /* Zona morta: joystick proximo ao centro retorna 90 graus */
    if (adc_val >= (centro - DEADZONE) && adc_val <= (centro + DEADZONE)) {
        return SERVO_MID;
    }

    int angulo = SERVO_MID;

    if (adc_val < (centro - DEADZONE)) {
        /* Abaixo do centro: mapeia para [SERVO_MIN .. SERVO_MID] */
        int range_adc = (centro - DEADZONE);
        if (range_adc > 0) {
            int range_ang = SERVO_MID - SERVO_MIN;
            angulo = SERVO_MID - ((centro - DEADZONE - adc_val) * range_ang) / range_adc;
        }
    } else {
        /* Acima do centro: mapeia para [SERVO_MID .. SERVO_MAX] */
        int range_adc = 4095 - (centro + DEADZONE);
        if (range_adc > 0) {
            int range_ang = SERVO_MAX - SERVO_MID;
            angulo = SERVO_MID + ((adc_val - centro - DEADZONE) * range_ang) / range_adc;
        }
    }

    if (angulo < SERVO_MIN) angulo = SERVO_MIN;
    if (angulo > SERVO_MAX) angulo = SERVO_MAX;
    return angulo;
}

static uint32_t angulo_para_duty(int graus)
{
    if (graus < 0)   graus = 0;
    if (graus > 180) graus = 180;
    int pulso_us = 500 + (graus * 2000) / 180;
    return ((uint32_t)pulso_us * 16383) / 20000;
}

/**
 * Move o servo suavemente: limita a mudanca de angulo por ciclo.
 * Evita movimentos bruscos que jogam a esfera para fora da mesa.
 */
static int suavizar_angulo(int angulo_atual, int angulo_alvo)
{
    int diff = angulo_alvo - angulo_atual;
    if (diff > SERVO_MAX_STEP)  diff = SERVO_MAX_STEP;
    if (diff < -SERVO_MAX_STEP) diff = -SERVO_MAX_STEP;
    return angulo_atual + diff;
}

/**
 * Aplica o offset mecanico e satura em 0-180 antes de gerar o pulso.
 * O offset entra AQUI (no fim da cadeia), depois de toda a logica
 * de zona morta e suavizacao ja terem rodado com angulos "limpos".
 */
static void set_servo1(int graus)
{
    int graus_ajustado = graus + SERVO1_OFFSET;
    if (graus_ajustado < 0)   graus_ajustado = 0;
    if (graus_ajustado > 180) graus_ajustado = 180;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, angulo_para_duty(graus_ajustado));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void set_servo2(int graus)
{
    int graus_ajustado = graus + SERVO2_OFFSET;
    if (graus_ajustado < 0)   graus_ajustado = 0;
    if (graus_ajustado > 180) graus_ajustado = 180;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, angulo_para_duty(graus_ajustado));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/* =========================================================
   FUNCOES AUXILIARES — MPU6050
   ========================================================= */

static esp_err_t mpu_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(mpu_dev, &reg, 1,
                                       buf, len, pdMS_TO_TICKS(100));
}

static bool mpu_le_giroscopio(float *gx, float *gy)
{
    uint8_t buf[6];
    if (mpu_read_reg(0x43, buf, 6) != ESP_OK) return false;
    int16_t gx_r = (int16_t)((buf[0] << 8) | buf[1]);
    int16_t gy_r = (int16_t)((buf[2] << 8) | buf[3]);
    *gx = gx_r / GYRO_SENS;
    *gy = gy_r / GYRO_SENS;
    return true;
}

static void mpu_calibrar_giroscopio(void)
{
    printf("Calibrando giroscopio... NAO MOVA O SENSOR!\n");
    float soma_gx = 0.0f, soma_gy = 0.0f;
    int validas = 0;
    for (int i = 0; i < CALIB_AMOSTRAS; i++) {
        float gx, gy;
        if (mpu_le_giroscopio(&gx, &gy)) {
            soma_gx += gx;
            soma_gy += gy;
            validas++;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    if (validas > 0) {
        gyro_bias_x = soma_gx / (float)validas;
        gyro_bias_y = soma_gy / (float)validas;
    }
    printf("Calibracao giroscopio OK (%d amostras).\n", validas);
    printf("  Bias X: %+.3f  |  Bias Y: %+.3f graus/s\n",
           gyro_bias_x, gyro_bias_y);
}

/* =========================================================
   MEDIA MOVEL
   ========================================================= */

typedef struct {
    float buf[MEDIA_AMOSTRAS];
    int   idx;
    float soma;
    int   cheio;
} media_movel_t;

static void media_init(media_movel_t *m)
{
    for (int i = 0; i < MEDIA_AMOSTRAS; i++) m->buf[i] = 0.0f;
    m->idx = 0; m->soma = 0.0f; m->cheio = 0;
}

static float media_add(media_movel_t *m, float val)
{
    m->soma -= m->buf[m->idx];
    m->buf[m->idx] = val;
    m->soma += val;
    m->idx = (m->idx + 1) % MEDIA_AMOSTRAS;
    if (m->idx == 0) m->cheio = 1;
    int n = m->cheio ? MEDIA_AMOSTRAS : (m->idx == 0 ? MEDIA_AMOSTRAS : m->idx);
    return m->soma / (float)n;
}

/* =========================================================
   CALIBRACAO DO JOYSTICK
   ========================================================= */

static void calibrar_joystick(void)
{
    printf("Calibrando joystick... NAO TOQUE NO JOYSTICK!\n");
    int soma_x = 0, soma_y = 0;
    for (int i = 0; i < 50; i++) {
        int lx = 0, ly = 0;
        adc_oneshot_read(adc_handle, JOY_X_CHANNEL, &lx);
        adc_oneshot_read(adc_handle, JOY_Y_CHANNEL, &ly);
        soma_x += lx;
        soma_y += ly;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    joy_center_x = soma_x / 50;
    joy_center_y = soma_y / 50;
    printf("Calibracao joystick OK.\n");
    printf("  Centro X: %d  |  Centro Y: %d\n", joy_center_x, joy_center_y);
    printf("  Zona morta: +/-%d  |  Range servo: %d a %d graus\n",
           DEADZONE, SERVO_MIN, SERVO_MAX);
}

/* =========================================================
   INICIALIZACOES
   ========================================================= */

void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(LED_PIN, 0);
    printf("LED inicializado (GPIO%d)\n", LED_PIN);
}

void btn_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << JOY_BTN_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    printf("Botao inicializado (GPIO%d)\n", JOY_BTN_PIN);
}

void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_USED,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));
    adc_oneshot_chan_cfg_t ch_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_X_CHANNEL, &ch_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_Y_CHANNEL, &ch_cfg));
    printf("ADC inicializado — GPIO4 e GPIO5\n");
}

void servo_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch1 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = SERVO1_PIN,
        .duty       = angulo_para_duty(SERVO_MID),
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch1));

    ledc_channel_config_t ch2 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = SERVO2_PIN,
        .duty       = angulo_para_duty(SERVO_MID),
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch2));

    printf("Servos inicializados — GPIO%d e GPIO%d\n", SERVO1_PIN, SERVO2_PIN);
}

void mpu6050_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_NUM_0,
        .scl_io_num        = I2C_SCL_PIN,
        .sda_io_num        = I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t addrs[2] = {0x68, 0x69};
    esp_err_t ret = ESP_FAIL;

    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addrs[i],
            .scl_speed_hz    = 100000,
        };
        ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &mpu_dev);
        if (ret != ESP_OK) continue;

        uint8_t wake[2] = {0x6B, 0x00};
        ret = i2c_master_transmit(mpu_dev, wake, 2, pdMS_TO_TICKS(200));
        if (ret == ESP_OK) {
            uint8_t dlpf[2] = {0x1A, 0x03};
            i2c_master_transmit(mpu_dev, dlpf, 2, pdMS_TO_TICKS(100));
            printf("MPU6050 inicializado — 0x%02X\n", addrs[i]);
            mpu_ok = true;
            break;
        }
        i2c_master_bus_rm_device(mpu_dev);
    }

    if (!mpu_ok) {
        printf("ERRO: MPU6050 nao encontrado!\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    mpu_calibrar_giroscopio();
}

/* =========================================================
   TASK 1 — JOYSTICK + BOTAO
   ========================================================= */

void task_joystick(void *arg)
{
    int lx = 0, ly = 0;
    bool btn_anterior = false;

    while (1) {
        adc_oneshot_read(adc_handle, JOY_X_CHANNEL, &lx);
        adc_oneshot_read(adc_handle, JOY_Y_CHANNEL, &ly);

        bool btn_agora = (gpio_get_level(JOY_BTN_PIN) == 0);
        if (btn_agora && !btn_anterior) {
            if (!jogo_ativo) {
                jogo_ativo     = true;
                jogo_inicio_us = esp_timer_get_time();
                printf("\n*** CRONOMETRO INICIADO! ***\n\n");
            } else {
                jogo_ativo = false;
                int64_t dur = esp_timer_get_time() - jogo_inicio_us;
                ultimo_tempo_s = (float)dur / 1000000.0f;
                printf("\n*** TEMPO FINAL: %.2f segundos ***\n\n", ultimo_tempo_s);
            }
        }
        btn_anterior = btn_agora;

        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            joy_x = lx;
            joy_y = ly;
            xSemaphoreGive(mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =========================================================
   TASK 2 — SERVOS com suavizacao
   ========================================================= */

void task_servos(void *arg)
{
    int lx = 2048, ly = 2048;
    int angulo1_atual = SERVO_MID;
    int angulo2_atual = SERVO_MID;

    while (1) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lx = joy_x;
            ly = joy_y;
            xSemaphoreGive(mutex);
        }

        int alvo1 = adc_para_angulo(lx, joy_center_x);
        int alvo2 = adc_para_angulo(ly, joy_center_y);

        /* Suaviza: move no maximo SERVO_MAX_STEP graus por ciclo */
        angulo1_atual = suavizar_angulo(angulo1_atual, alvo1);
        angulo2_atual = suavizar_angulo(angulo2_atual, alvo2);

        set_servo1(angulo1_atual);
        set_servo2(angulo2_atual);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =========================================================
   TASK 3 — MPU6050
   ========================================================= */

void task_mpu6050(void *arg)
{
    if (!mpu_ok) {
        printf("Task MPU6050 encerrada.\n");
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[6] = {0};
    float pitch = 0.0f, roll = 0.0f;
    const float dt = 0.01f;
    media_movel_t mm_pitch, mm_roll;
    media_init(&mm_pitch);
    media_init(&mm_roll);

    /* =====================================================
       FASE 1 — Convergencia do filtro complementar.
       O filtro comeca em 0,0 e precisa de algumas iteracoes
       para "alcancar" o angulo real antes de calibrar o bias.
       Roda silenciosamente, sem usar esses valores ainda.
       ===================================================== */
    for (int i = 0; i < 100; i++) {
        float ax = 0, ay = 0, az = 0, gx = 0, gy = 0;

        if (mpu_read_reg(0x3B, buf, 6) == ESP_OK) {
            ax = (int16_t)((buf[0]<<8)|buf[1]) / ACCEL_SENS;
            ay = (int16_t)((buf[2]<<8)|buf[3]) / ACCEL_SENS;
            az = (int16_t)((buf[4]<<8)|buf[5]) / ACCEL_SENS;
        }
        if (mpu_read_reg(0x43, buf, 6) == ESP_OK) {
            gx = (int16_t)((buf[0]<<8)|buf[1]) / GYRO_SENS - gyro_bias_x;
            gy = (int16_t)((buf[2]<<8)|buf[3]) / GYRO_SENS - gyro_bias_y;
        }

        float pa = atan2f(ax, sqrtf(ay*ay+az*az)) * 180.0f / (float)M_PI;
        float ra = atan2f(ay, sqrtf(ax*ax+az*az)) * 180.0f / (float)M_PI;
        pitch = FILTRO_ALPHA*(pitch+gx*dt) + (1.0f-FILTRO_ALPHA)*pa;
        roll  = FILTRO_ALPHA*(roll +gy*dt) + (1.0f-FILTRO_ALPHA)*ra;

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* =====================================================
       FASE 2 — Calibracao do "zero" (nivelamento).
       Mesa deve estar parada e nivelada. Acumula pitch/roll
       ja filtrados e calcula a media como bias de offset.
       ===================================================== */
    printf("Calibrando nivelamento do MPU6050... MESA DEVE ESTAR RETA!\n");
    float soma_pitch = 0.0f, soma_roll = 0.0f;

    for (int i = 0; i < CALIB_NIVEL_AMOSTRAS; i++) {
        float ax = 0, ay = 0, az = 0, gx = 0, gy = 0;

        if (mpu_read_reg(0x3B, buf, 6) == ESP_OK) {
            ax = (int16_t)((buf[0]<<8)|buf[1]) / ACCEL_SENS;
            ay = (int16_t)((buf[2]<<8)|buf[3]) / ACCEL_SENS;
            az = (int16_t)((buf[4]<<8)|buf[5]) / ACCEL_SENS;
        }
        if (mpu_read_reg(0x43, buf, 6) == ESP_OK) {
            gx = (int16_t)((buf[0]<<8)|buf[1]) / GYRO_SENS - gyro_bias_x;
            gy = (int16_t)((buf[2]<<8)|buf[3]) / GYRO_SENS - gyro_bias_y;
        }

        float pa = atan2f(ax, sqrtf(ay*ay+az*az)) * 180.0f / (float)M_PI;
        float ra = atan2f(ay, sqrtf(ax*ax+az*az)) * 180.0f / (float)M_PI;
        pitch = FILTRO_ALPHA*(pitch+gx*dt) + (1.0f-FILTRO_ALPHA)*pa;
        roll  = FILTRO_ALPHA*(roll +gy*dt) + (1.0f-FILTRO_ALPHA)*ra;

        soma_pitch += pitch;
        soma_roll  += roll;

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    pitch_bias = soma_pitch / (float)CALIB_NIVEL_AMOSTRAS;
    roll_bias  = soma_roll  / (float)CALIB_NIVEL_AMOSTRAS;

    printf("Calibracao de nivelamento OK.\n");
    printf("  Pitch bias: %+.2f graus  |  Roll bias: %+.2f graus\n",
           pitch_bias, roll_bias);

    /* =====================================================
       LOOP PRINCIPAL — aplica o bias de nivelamento em
       toda leitura final antes de salvar no estado global.
       ===================================================== */
    while (1) {
        float ax = 0, ay = 0, az = 0, gx = 0, gy = 0;

        if (mpu_read_reg(0x3B, buf, 6) == ESP_OK) {
            ax = (int16_t)((buf[0]<<8)|buf[1]) / ACCEL_SENS;
            ay = (int16_t)((buf[2]<<8)|buf[3]) / ACCEL_SENS;
            az = (int16_t)((buf[4]<<8)|buf[5]) / ACCEL_SENS;
        }
        if (mpu_read_reg(0x43, buf, 6) == ESP_OK) {
            gx = (int16_t)((buf[0]<<8)|buf[1]) / GYRO_SENS - gyro_bias_x;
            gy = (int16_t)((buf[2]<<8)|buf[3]) / GYRO_SENS - gyro_bias_y;
        }

        float pa = atan2f(ax, sqrtf(ay*ay+az*az)) * 180.0f / (float)M_PI;
        float ra = atan2f(ay, sqrtf(ax*ax+az*az)) * 180.0f / (float)M_PI;
        pitch = FILTRO_ALPHA*(pitch+gx*dt) + (1.0f-FILTRO_ALPHA)*pa;
        roll  = FILTRO_ALPHA*(roll +gy*dt) + (1.0f-FILTRO_ALPHA)*ra;

        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            mpu_data.pitch = media_add(&mm_pitch, pitch - pitch_bias);
            mpu_data.roll  = media_add(&mm_roll,  roll  - roll_bias);
            xSemaphoreGive(mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* =========================================================
   TASK 4 — CONSOLE + JSON
   ========================================================= */

void task_console(void *arg)
{
    int   lx = 2048, ly = 2048;
    float pitch = 0.0f, roll = 0.0f;

    while (1) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lx = joy_x; ly = joy_y;
            pitch = mpu_data.pitch; roll = mpu_data.roll;
            xSemaphoreGive(mutex);
        }

        int s1 = adc_para_angulo(lx, joy_center_x);
        int s2 = adc_para_angulo(ly, joy_center_y);

        float tempo_atual = ultimo_tempo_s;
        if (jogo_ativo)
            tempo_atual = (float)(esp_timer_get_time()-jogo_inicio_us)/1000000.0f;

        const char *pp = "RETO     ", *pr = "RETO     ";
        if      (pitch >  10.0f) pp = "ATRAS    ";
        else if (pitch < -10.0f) pp = "FRENTE   ";
        if      (roll  >  10.0f) pr = "ESQUERDA ";
        else if (roll  < -10.0f) pr = "DIREITA  ";

        printf("\n==================================================\n");
        printf("  MESA LABIRINTO - ESP32-S3\n");
        printf("  [zona morta: +/-%d  |  step: %d  |  range: %d-%d]\n",
               DEADZONE, SERVO_MAX_STEP, SERVO_MIN, SERVO_MAX);
        printf("--------------------------------------------------\n");
        printf("  JOY X: %4d (centro:%d) -> SERVO1: %3d graus\n",
               lx, joy_center_x, s1);
        printf("  JOY Y: %4d (centro:%d) -> SERVO2: %3d graus\n",
               ly, joy_center_y, s2);
        printf("--------------------------------------------------\n");
        if (mpu_ok) {
            printf("  Pitch: %+6.1f graus  | %s\n", pitch, pp);
            printf("  Roll : %+6.1f graus  | %s\n", roll,  pr);
        } else {
            printf("  MPU6050 NAO CONECTADO\n");
        }
        printf("--------------------------------------------------\n");
        if (jogo_ativo)
            printf("  CRONOMETRO: %.1f s (em andamento...)\n", tempo_atual);
        else if (ultimo_tempo_s > 0)
            printf("  ULTIMA PARTIDA: %.2f s\n", ultimo_tempo_s);
        else
            printf("  Pressione o botao para iniciar!\n");
        printf("==================================================\n");

        printf("DATA:{\"pitch\":%.2f,\"roll\":%.2f,\"joy_x\":%d,\"joy_y\":%d,"
               "\"servo1\":%d,\"servo2\":%d,\"tempo_partida\":%.2f,\"jogo_ativo\":%d}\n",
               pitch, roll, lx, ly, s1, s2, tempo_atual, jogo_ativo ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* =========================================================
   APP MAIN
   ========================================================= */

void app_main(void)
{
    printf("\n==================================================\n");
    printf("  MESA LABIRINTO + GEMEO DIGITAL\n");
    printf("  ESP32-S3\n");
    printf("==================================================\n\n");

    mutex = xSemaphoreCreateMutex();
    led_init();
    btn_init();
    adc_init();
    servo_init();
    calibrar_joystick();
    mpu6050_init();

    gpio_set_level(LED_PIN, 1);
    printf("\nSistema pronto!\n");
    printf("  Mova o joystick para controlar os servos.\n");
    printf("  Pressione o botao para iniciar/parar o cronometro.\n\n");

    xTaskCreate(task_joystick, "Task_Joystick", 2048, NULL, 5, NULL);
    xTaskCreate(task_servos,   "Task_Servos",   2048, NULL, 5, NULL);
    xTaskCreate(task_mpu6050,  "Task_MPU6050",  4096, NULL, 5, NULL);
    xTaskCreate(task_console,  "Task_Console",  4096, NULL, 3, NULL);
}