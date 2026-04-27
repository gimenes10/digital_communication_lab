/**
 * @file  bh1750.c
 * @brief Implementação do driver BH1750FVI usando I2C master (ESP-IDF v6.x)
 *
 * Fluxo de uma leitura no modo One-Time High-Res:
 *
 *   ┌──────────┐   power_on(0x01)   ┌─────────────┐
 *   │ POWER_DN │ ─────────────────> │   POWER_ON  │
 *   └──────────┘                    └─────────────┘
 *                                          │
 *                                          │ measure_cmd(0x20)
 *                                          ▼
 *                                   ┌─────────────┐
 *                                   │ MEASURING   │ ~120ms
 *                                   └─────────────┘
 *                                          │
 *                                          │ (auto, em One-Time)
 *                                          ▼
 *                                   ┌─────────────┐
 *                                   │  POWER_DN   │
 *                                   └─────────────┘
 *
 * Após o tempo de conversão, lemos 2 bytes (MSB primeiro) e dividimos
 * por 1.2 para obter o lux (conforme datasheet, página 11).
 */

#include "bh1750.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bh1750";

/* ── Pinagem e endereçamento ─────────────────────────────────────── */
#define BH1750_SDA_PIN       GPIO_NUM_47
#define BH1750_SCL_PIN       GPIO_NUM_48
#define BH1750_I2C_PORT      I2C_NUM_1            /* Bus separado do OLED */
#define BH1750_I2C_ADDR      0x23                 /* ADDR pin = GND */
#define BH1750_I2C_FREQ_HZ   100000               /* Standard 100kHz */
#define BH1750_I2C_TIMEOUT   100                  /* ms para transações */

/* ── Comandos do BH1750 (datasheet pg. 5) ────────────────────────── */
#define BH1750_CMD_POWER_DOWN         0x00
#define BH1750_CMD_POWER_ON           0x01
#define BH1750_CMD_RESET              0x07          /* Limpa registrador de dado */
#define BH1750_CMD_ONE_TIME_H_RES     0x20          /* 1 lx, ~120ms, auto-sleep */

/* ── Tempo de conversão (datasheet pg. 2, Tabela "Measurement Time") ── */
#define BH1750_CONV_TIME_MS           180           /* H-Res máx, com margem */

/* ── Handles do barramento I2C (alocados em bh1750_init) ─────────── */
static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;

/**
 * Envia um único byte de comando ao BH1750.
 * O BH1750 não usa "registradores" — todo controle é via opcode de 1 byte.
 */
static esp_err_t bh1750_send_cmd(uint8_t cmd)
{
    return i2c_master_transmit(s_dev_handle, &cmd, 1, BH1750_I2C_TIMEOUT);
}

esp_err_t bh1750_init(void)
{
    esp_err_t ret;

    /* ── 1. Cria o barramento I2C_NUM_1 ──────────────────────────── */
    /* Diferente do OLED: usamos um bus separado pra evitar contenção
       e simplificar timing. O ESP32-S3 tem 2 controladores I2C HW. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BH1750_I2C_PORT,
        .sda_io_num        = BH1750_SDA_PIN,
        .scl_io_num        = BH1750_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,                  /* Filtro de ruído padrão */
        .flags.enable_internal_pullup = true,    /* Backup; idealmente use pull-ups externos 4.7k */
    };

    ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar bus I2C: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ── 2. Adiciona o BH1750 como dispositivo do bus ────────────── */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BH1750_I2C_ADDR,
        .scl_speed_hz    = BH1750_I2C_FREQ_HZ,
    };

    ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao adicionar device: %s", esp_err_to_name(ret));
        goto fail_remove_bus;
    }

    /* ── 3. Sequência de inicialização do sensor ──────────────────── */
    /* Power-on é necessário pois o sensor inicia em power-down após VCC. */
    ret = bh1750_send_cmd(BH1750_CMD_POWER_ON);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Power-on falhou: %s", esp_err_to_name(ret));
        goto fail_remove_dev;
    }

    /* Reset zera o data register (boa prática após power-on). */
    ret = bh1750_send_cmd(BH1750_CMD_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset falhou: %s", esp_err_to_name(ret));
        goto fail_remove_dev;
    }

    ESP_LOGI(TAG, "BH1750 inicializado em SDA=%d SCL=%d addr=0x%02X",
             BH1750_SDA_PIN, BH1750_SCL_PIN, BH1750_I2C_ADDR);
    return ESP_OK;

    /* ── Cleanup em caso de erro ─────────────────────────────────── */
fail_remove_dev:
    i2c_master_bus_rm_device(s_dev_handle);
    s_dev_handle = NULL;
fail_remove_bus:
    i2c_del_master_bus(s_bus_handle);
    s_bus_handle = NULL;
fail:
    return ret;
}

esp_err_t bh1750_read_lux(uint16_t *lux_out)
{
    esp_err_t ret;
    uint8_t   raw_data[2];

    if (lux_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_dev_handle == NULL) {
        ESP_LOGE(TAG, "Driver nao inicializado — chame bh1750_init primeiro");
        return ESP_ERR_INVALID_STATE;
    }

    /* ── 1. Dispara a medição (modo One-Time H-Res) ──────────────── */
    /* Após este comando, o sensor "acorda" do power-down, mede por ~120ms,
       guarda o resultado e volta automaticamente para power-down. */
    ret = bh1750_send_cmd(BH1750_CMD_ONE_TIME_H_RES);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Comando measure falhou: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ── 2. Aguarda conversão completar ──────────────────────────── */
    /* Datasheet: max 180ms para High-Res. Usamos esse valor com margem. */
    vTaskDelay(pdMS_TO_TICKS(BH1750_CONV_TIME_MS));

    /* ── 3. Lê os 2 bytes do data register (MSB primeiro) ────────── */
    /* O BH1750 entrega o resultado em formato big-endian:
       byte[0] = MSB, byte[1] = LSB. */
    ret = i2c_master_receive(s_dev_handle, raw_data, 2, BH1750_I2C_TIMEOUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Leitura I2C falhou: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* ── 4. Compõe valor cru de 16 bits e converte para lux ──────── */
    uint16_t raw_value = ((uint16_t)raw_data[0] << 8) | raw_data[1];

    /* Datasheet pg. 11: lux = raw / 1.2
       Dividir antes de cast para uint16 evita overflow na multiplicação.
       O resultado max é 65535/1.2 = 54612 lx, cabendo confortavelmente. */
    *lux_out = (uint16_t)((float)raw_value / 1.2f);

    return ESP_OK;

fail:
    *lux_out = 0;
    return ret;
}
