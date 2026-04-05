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

static const char *TAG = "sensor";

/* ── Protocolo ───────────────────────────────────────────────────── */
#define HEADER_REQUEST   0xBB
#define CMD_READ_SENSOR  0x01
#define HEADER_RESPONSE  0xAA
#define REQUEST_LEN      2
#define RESPONSE_LEN     4

/* ── Simulação do LDR ────────────────────────────────────────────── */
static uint16_t s_ldr_base = 2048;  /* Valor base simulado (meio da escala) */

/**
 * @brief Simula uma leitura de ADC do LDR (0–4095).
 *
 * Gera um valor com drift lento e ruído, imitando um sensor real.
 * Para usar o ADC real, substituir por:
 *   adc_oneshot_read(adc_handle, ADC_CHANNEL_X, &adc_raw);
 */
static uint16_t simulate_ldr_reading(void)
{
    /* Drift lento: ±32 por leitura */
    int32_t drift = ((int32_t)(esp_random() % 65) - 32);
    int32_t base  = (int32_t)s_ldr_base + drift;

    /* Clamp na faixa ADC 12-bit */
    if (base < 0)    base = 0;
    if (base > 4095)  base = 4095;
    s_ldr_base = (uint16_t)base;

    /* Ruído: ±50 */
    int32_t noise = ((int32_t)(esp_random() % 101) - 50);
    int32_t value = base + noise;

    if (value < 0)    value = 0;
    if (value > 4095)  value = 4095;

    return (uint16_t)value;
}

/* ── Task principal ──────────────────────────────────────────────── */
static void sensor_task(void *arg)
{
    ssd1306_t *oled = (ssd1306_t *)arg;
    sx1262_t radio;
    esp_err_t ret;
    char line[22];  /* 128px / 6px per char = 21 chars max */

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
        uint16_t adc_value = simulate_ldr_reading();
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
