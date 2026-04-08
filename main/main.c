/**
 * @file main.c
 * @brief LoRa Sensor Node (Heltec #1) — Responder
 *
 * Aguarda pacotes LoRa de requisição do gateway.
 * Ao receber [0xBB, 0x01], gera um valor de LDR simulado
 * e responde com [0xAA, DATA_HIGH, DATA_LOW, XOR].
 *
 * No sistema final, o valor virá de um LDR real no ADC.
 * Nesta versão piloto, o valor é simulado com esp_random().
 *
 * Plataforma: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
 * Framework:  ESP-IDF v6.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sx1262.h"
#include "ssd1306.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "sensor";

/* ── Protocolo ───────────────────────────────────────────────────── */
#define HEADER_REQUEST   0xBB
#define CMD_READ_SENSOR  0x01
#define HEADER_RESPONSE  0xAA
#define REQUEST_LEN      2
#define RESPONSE_LEN     4

/* ── ADC para LDR (GPIO1 = ADC1_CH0) ────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc_handle;

/**
 * @brief Inicializa o ADC1 canal 0 (GPIO1) para leitura do LDR.
 *
 * Configura atenuação de 12 dB (faixa ~0–3.1 V) e resolução de 12 bits
 * (valores de 0 a 4095). O LDR deve estar conectado ao GPIO1 com um
 * divisor resistivo.
 *
 * @return ESP_OK em caso de sucesso.
 */
static esp_err_t ldr_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };

    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) return ret;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    return adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_0, &chan_cfg);
}

/**
 * @brief Lê o valor bruto do LDR via ADC (0–4095).
 *
 * Retorna 0 em caso de falha na leitura.
 */
static uint16_t read_ldr(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, ADC_CHANNEL_0, &raw);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return 0;
    }
    return (uint16_t)raw;
}

/* ── Task principal ──────────────────────────────────────────────── */

/**
 * @brief Task FreeRTOS do nó sensor (responder LoRa).
 *
 * Fluxo:
 *  1. Inicializa ADC (LDR) e rádio SX1262.
 *  2. Entra em RX contínuo aguardando requisições do gateway.
 *  3. Ao receber um pacote válido [0xBB, 0x01]:
 *     - Lê o valor do LDR via ADC.
 *     - Monta resposta [0xAA, DATA_H, DATA_L, XOR] e transmite.
 *     - Atualiza o display OLED com o valor enviado e o RSSI.
 *
 * @param arg  Ponteiro para ssd1306_t (display OLED) ou NULL se indisponível.
 */
static void sensor_task(void *arg)
{
    ssd1306_t *oled = (ssd1306_t *)arg;
    sx1262_t radio;
    esp_err_t ret;
    char line[22];  /* 128px / 6px per char = 21 chars max */

    ret = ldr_adc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ADC: %s", esp_err_to_name(ret));
    }

    ret = sx1262_init(&radio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar SX1262");
        if (oled) {
            ssd1306_clear(oled);
            ssd1306_draw_string(oled, 0, 0, "SX1262 INIT FAIL");
            ssd1306_update(oled);
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "=== LoRa Sensor Node (Responder) ===");
    ESP_LOGI(TAG, "Aguardando requisicoes do gateway...");

    if (oled) {
        ssd1306_clear(oled);
        ssd1306_draw_string(oled, 0, 0, "LoRa Sensor Node");
        ssd1306_draw_string(oled, 2, 0, "Aguardando...");
        ssd1306_update(oled);
    }

    while (1) {
        /* ── Entra em RX contínuo ────────────────────────────────── */
        ret = sx1262_receive_packet(&radio, 0);  /* 0 = contínuo */
        if (ret == ESP_ERR_TIMEOUT) {
            /* Em RX contínuo não deveria dar timeout, mas tratamos */
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Erro RX: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ── Lê pacote recebido ──────────────────────────────────── */
        uint8_t rx_data[LORA_MAX_PAYLOAD];
        uint8_t rx_len = 0;
        int16_t rssi = 0;

        ret = sx1262_read_packet(&radio, rx_data, &rx_len, &rssi);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Erro ao ler pacote: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "Pacote recebido (%d bytes, RSSI: %d dBm)", rx_len, rssi);

        /* ── Valida request ──────────────────────────────────────── */
        if (rx_len != REQUEST_LEN) {
            ESP_LOGW(TAG, "Tamanho invalido: %d (esperado %d)", rx_len, REQUEST_LEN);
            continue;
        }

        if (rx_data[0] != HEADER_REQUEST || rx_data[1] != CMD_READ_SENSOR) {
            ESP_LOGW(TAG, "Request invalido: [0x%02X 0x%02X]", rx_data[0], rx_data[1]);
            continue;
        }

        ESP_LOGI(TAG, "Request valido recebido! Lendo sensor...");

        /* ── Gera valor do sensor ────────────────────────────────── */
        uint16_t adc_value = read_ldr();
        uint8_t data_high  = (uint8_t)(adc_value >> 8);
        uint8_t data_low   = (uint8_t)(adc_value & 0xFF);
        uint8_t checksum   = data_high ^ data_low;

        /* ── Monta e envia pacote de resposta ────────────────────── */
        uint8_t response[] = {
            HEADER_RESPONSE,
            data_high,
            data_low,
            checksum,
        };

        /* Pequeno delay para garantir que o gateway já entrou em RX */
        vTaskDelay(pdMS_TO_TICKS(50));

        ret = sx1262_send_packet(&radio, response, RESPONSE_LEN);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao enviar resposta: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "Resposta enviada: LDR=%u [0x%02X 0x%02X 0x%02X 0x%02X]",
                 adc_value, response[0], response[1], response[2], response[3]);

        if (oled) {
            ssd1306_clear(oled);
            ssd1306_draw_string(oled, 0, 0, "LoRa Sensor Node");

            snprintf(line, sizeof(line), "LDR: %u", adc_value);
            ssd1306_draw_string(oled, 2, 0, line);

            snprintf(line, sizeof(line), "RSSI: %d dBm", rssi);
            ssd1306_draw_string(oled, 4, 0, line);

            ssd1306_draw_string(oled, 6, 0, "Enviado OK");
            ssd1306_update(oled);
        }
    }
}

/**
 * @brief Ponto de entrada da aplicação ESP-IDF.
 *
 * Inicializa o display OLED (falha é não-fatal) e cria a task do sensor
 * no core 1 com 4 KB de stack.
 */
void app_main(void)
{
    ssd1306_t *oled = NULL;
    esp_err_t ret = ssd1306_init(&oled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED init falhou (%s) — continuando sem display",
                 esp_err_to_name(ret));
    }

    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, oled, 5, NULL, 1);
}
