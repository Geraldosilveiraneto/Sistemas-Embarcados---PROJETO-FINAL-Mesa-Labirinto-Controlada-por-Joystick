#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

/* =========================================================
   PINAGEM
   ========================================================= */

#define JOY_X_CHANNEL   ADC_CHANNEL_3
#define JOY_Y_CHANNEL   ADC_CHANNEL_4
#define ADC_UNIT_USED   ADC_UNIT_1

#define SERVO1_PIN      GPIO_NUM_42
#define SERVO2_PIN      GPIO_NUM_41
#define PWM_FREQ_HZ     50

#define LED_PIN         GPIO_NUM_48

#define I2C_SDA_PIN     GPIO_NUM_17
#define I2C_SCL_PIN     GPIO_NUM_18
#define ACCEL_SENS      16384.0f
#define GYRO_SENS       131.0f

/* =========================================================
   FILTRO — AJUSTE AQUI SE AINDA OSCILAR
   Alpha próximo de 1.0 = mais suave (mais giroscópio)
   Alpha próximo de 0.0 = mais rápido (mais acelerômetro)
   ========================================================= */
#define FILTRO_ALPHA    0.98f     // 98% giroscópio + 2% acelerômetro

/* Média móvel: quantas amostras usar para suavizar (4 a 16) */
#define MEDIA_AMOSTRAS  8

/* =========================================================
   VARIÁVEIS COMPARTILHADAS
   ========================================================= */

static SemaphoreHandle_t mutex              = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static i2c_master_bus_handle_t   i2c_bus;
static i2c_master_dev_handle_t   mpu_dev;
static bool mpu_ok = false;

static int joy_x = 2048;
static int joy_y = 2048;

typedef struct {
    float pitch;
    float roll;
} mpu_data_t;

static mpu_data_t mpu_data = {0.0f, 0.0f};

/* =========================================================
   FUNÇÕES AUXILIARES — SERVO
   ========================================================= */

static int adc_para_angulo(int adc)
{
    if (adc < 0)    adc = 0;
    if (adc > 4095) adc = 4095;
    return (adc * 180) / 4095;
}

static uint32_t angulo_para_duty(int graus)
{
    if (graus < 0)   graus = 0;
    if (graus > 180) graus = 180;
    int pulso_us = 500 + (graus * 2000) / 180;
    return ((uint32_t)pulso_us * 16383) / 20000;
}

static void set_servo1(int graus)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, angulo_para_duty(graus));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void set_servo2(int graus)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, angulo_para_duty(graus));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/* =========================================================
   FUNÇÕES AUXILIARES — MPU6050
   ========================================================= */

static esp_err_t mpu_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(mpu_dev, &reg, 1,
                                       buf, len, pdMS_TO_TICKS(100));
}

/* =========================================================
   MÉDIA MÓVEL — suaviza oscilações residuais
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
    m->idx   = 0;
    m->soma  = 0.0f;
    m->cheio = 0;
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
   INICIALIZAÇÕES
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
    printf("✓ LED inicializado (GPIO%d)\n", LED_PIN);
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
    printf("✓ ADC inicializado — GPIO4 e GPIO5\n");
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
        .duty       = angulo_para_duty(90),
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch1));

    ledc_channel_config_t ch2 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = SERVO2_PIN,
        .duty       = angulo_para_duty(90),
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch2));

    printf("✓ Servos inicializados — GPIO%d e GPIO%d\n", SERVO1_PIN, SERVO2_PIN);
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
    esp_err_t ret    = ESP_FAIL;

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
            // Configura filtro passa-baixa interno do MPU6050
            // Registro 0x1A: DLPF_CFG = 3 (44Hz) — reduz ruído do sensor
            uint8_t dlpf[2] = {0x1A, 0x03};
            i2c_master_transmit(mpu_dev, dlpf, 2, pdMS_TO_TICKS(100));

            printf("✓ MPU6050 inicializado — endereco 0x%02X\n", addrs[i]);
            mpu_ok = true;
            break;
        }
        i2c_master_bus_rm_device(mpu_dev);
    }

    if (!mpu_ok) {
        printf("✗ ERRO: MPU6050 nao encontrado!\n");
        printf("  Conecte: SDA->GPIO%d | SCL->GPIO%d | AD0->GND\n",
               I2C_SDA_PIN, I2C_SCL_PIN);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}

/* =========================================================
   TASK 1 — LEITURA DO JOYSTICK
   ========================================================= */

void task_joystick(void *arg)
{
    int lx = 0;
    int ly = 0;

    while (1) {
        adc_oneshot_read(adc_handle, JOY_X_CHANNEL, &lx);
        adc_oneshot_read(adc_handle, JOY_Y_CHANNEL, &ly);

        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            joy_x = lx;
            joy_y = ly;
            xSemaphoreGive(mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =========================================================
   TASK 2 — CONTROLE DOS SERVOS
   ========================================================= */

void task_servos(void *arg)
{
    int lx = 2048;
    int ly = 2048;

    while (1) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lx = joy_x;
            ly = joy_y;
            xSemaphoreGive(mutex);
        }

        set_servo1(adc_para_angulo(lx));
        set_servo2(adc_para_angulo(ly));

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =========================================================
   TASK 3 — LEITURA DO MPU6050
   Filtro complementar + média móvel
   ========================================================= */

void task_mpu6050(void *arg)
{
    if (!mpu_ok) {
        printf("⚠ Task MPU6050 encerrada — sensor nao encontrado.\n");
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[6]    = {0};
    float pitch       = 0.0f;
    float roll        = 0.0f;
    const float dt    = 0.01f;

    // Média móvel para pitch e roll
    media_movel_t mm_pitch;
    media_movel_t mm_roll;
    media_init(&mm_pitch);
    media_init(&mm_roll);

    // Calibração: descarta as primeiras 50 leituras para estabilizar
    printf("Calibrando MPU6050...\n");
    for (int i = 0; i < 50; i++) {
        mpu_read_reg(0x3B, buf, 6);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("✓ MPU6050 calibrado!\n");

    while (1) {
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        float gx = 0.0f, gy = 0.0f;

        // Lê acelerômetro
        if (mpu_read_reg(0x3B, buf, 6) == ESP_OK) {
            int16_t ax_r = (int16_t)((buf[0] << 8) | buf[1]);
            int16_t ay_r = (int16_t)((buf[2] << 8) | buf[3]);
            int16_t az_r = (int16_t)((buf[4] << 8) | buf[5]);
            ax = ax_r / ACCEL_SENS;
            ay = ay_r / ACCEL_SENS;
            az = az_r / ACCEL_SENS;
        }

        // Lê giroscópio
        if (mpu_read_reg(0x43, buf, 6) == ESP_OK) {
            int16_t gx_r = (int16_t)((buf[0] << 8) | buf[1]);
            int16_t gy_r = (int16_t)((buf[2] << 8) | buf[3]);
            gx = gx_r / GYRO_SENS;
            gy = gy_r / GYRO_SENS;
        }

        // Ângulos pelo acelerômetro
        float pitch_acc = atan2f(ax, sqrtf(ay*ay + az*az)) * 180.0f / (float)M_PI;
        float roll_acc  = atan2f(ay, sqrtf(ax*ax + az*az)) * 180.0f / (float)M_PI;

        // Filtro complementar
        pitch = FILTRO_ALPHA * (pitch + gx * dt) + (1.0f - FILTRO_ALPHA) * pitch_acc;
        roll  = FILTRO_ALPHA * (roll  + gy * dt) + (1.0f - FILTRO_ALPHA) * roll_acc;

        // Média móvel sobre o resultado final
        float pitch_suave = media_add(&mm_pitch, pitch);
        float roll_suave  = media_add(&mm_roll,  roll);

        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            mpu_data.pitch = pitch_suave;
            mpu_data.roll  = roll_suave;
            xSemaphoreGive(mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(10));   // 100Hz
    }
}

/* =========================================================
   TASK 4 — CONSOLE / DEBUG
   ========================================================= */

void task_console(void *arg)
{
    int   lx    = 2048;
    int   ly    = 2048;
    float pitch = 0.0f;
    float roll  = 0.0f;

    while (1) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lx    = joy_x;
            ly    = joy_y;
            pitch = mpu_data.pitch;
            roll  = mpu_data.roll;
            xSemaphoreGive(mutex);
        }

        const char *pos_pitch = "RETO     ";
        const char *pos_roll  = "RETO     ";
        if      (pitch >  10.0f) pos_pitch = "FRENTE   ";
        else if (pitch < -10.0f) pos_pitch = "ATRAS    ";
        if      (roll  >  10.0f) pos_roll  = "DIREITA  ";
        else if (roll  < -10.0f) pos_roll  = "ESQUERDA ";

        printf("\n╔══════════════════════════════════════════════════╗\n");
        printf("║         MESA LABIRINTO — ESP32-S3                ║\n");
        printf("╠══════════════════════════════════════════════════╣\n");
        printf("║  JOYSTICK  X: %4d -> SERVO1: %3d graus          ║\n",
               lx, adc_para_angulo(lx));
        printf("║  JOYSTICK  Y: %4d -> SERVO2: %3d graus          ║\n",
               ly, adc_para_angulo(ly));
        printf("╠══════════════════════════════════════════════════╣\n");
        if (mpu_ok) {
            printf("║  MPU6050   Pitch: %+6.1f graus  |  %-9s    ║\n",
                   pitch, pos_pitch);
            printf("║  MPU6050   Roll : %+6.1f graus  |  %-9s    ║\n",
                   roll, pos_roll);
        } else {
            printf("║  MPU6050   NAO CONECTADO                         ║\n");
            printf("║            SDA->GPIO%d  |  SCL->GPIO%d           ║\n",
                   I2C_SDA_PIN, I2C_SCL_PIN);
        }
        printf("╚══════════════════════════════════════════════════╝\n");

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* =========================================================
   APP MAIN
   ========================================================= */

void app_main(void)
{
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║   FASE 2 — JOYSTICK + SERVO + MPU6050           ║\n");
    printf("║   ESP32-S3                                       ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    mutex = xSemaphoreCreateMutex();

    led_init();
    adc_init();
    servo_init();
    mpu6050_init();

    gpio_set_level(LED_PIN, 1);
    printf("\n✓ Sistema pronto! Mova o joystick.\n\n");

    xTaskCreate(task_joystick, "Task_Joystick", 2048, NULL, 5, NULL);
    xTaskCreate(task_servos,   "Task_Servos",   2048, NULL, 5, NULL);
    xTaskCreate(task_mpu6050,  "Task_MPU6050",  4096, NULL, 5, NULL);
    xTaskCreate(task_console,  "Task_Console",  4096, NULL, 3, NULL);
}