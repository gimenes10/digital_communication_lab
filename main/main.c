/**
 * @file  main.c
 * @brief LoRa Sensor Node (Heltec #1) — Responder com BH1750
 *
 * NÓ SENSOR DO PIPELINE END-TO-END:
 *
 *   ┌─────────────┐  LoRa req   ┌──────────────┐  LoRa resp  ┌──────────┐
 *   │   Gateway   │ ──────────> │  ESTE NÓ     │ ──────────> │ Gateway  │
 *   │ dcl_request │             │  + BH1750    │             │  + UART  │ ───> FPGA
 *   └─────────────┘             └──────────────┘             └──────────┘
 *
 * Comportamento:
 *   1. Inicializa SX1262 (LoRa 915MHz), OLED e BH1750
 *   2. Entra em RX contínuo aguardando request
 *   3. Ao receber [0xBB, 0x01]:
 *        a. Dispara medição no BH1750 (~180ms)
 *        b. Monta resposta [0xAA, lux_HIGH, lux_LOW, XOR]
 *        c. Transmite via LoRa
 *        d. Atualiza OLED com lux + RSSI
 *   4. Volta para RX
 *
 *
 * Plataforma: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
 * Framework:  ESP-IDF v6.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sx1262.h"
#include "ssd1306.h"
#include "bh1750.h"

static const char *TAG = "sensor";

/* ── Protocolo Request/Response ──────────────────────────────────── */
/* Mantido igual à versão LDR para compatibilidade com o gateway     */
#define HEADER_REQUEST   0xBB    /* Byte 0 do request                  */
#define CMD_READ_SENSOR  0x01    /* Byte 1: comando "ler sensor"       */
#define HEADER_RESPONSE  0xAA    /* Byte 0 da resposta                 */
#define REQUEST_LEN      2       /* [HEADER, CMD]                      */
#define RESPONSE_LEN     4       /* [HEADER, HIGH, LOW, XOR]           */

/**
 * Task principal do nó sensor.
 *
 * Roda no Core 1 (separado do Core 0 que cuida de WiFi/BT stack).
 * É um loop infinito event-driven: bloqueia em sx1262_receive_packet
 * até chegar request, processa, responde, volta a esperar.
 */
static void sensor_task(void *arg)
{
    ssd1306_t *oled = (ssd1306_t *)arg;
    sx1262_t   radio;
    esp_err_t  ret;
    char       line[22];   /* OLED: 128px / 6px = 21 chars + \0 */

    /* ── Inicialização do sensor de luminosidade ─────────────────── */
    ret = bh1750_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 init falhou: %s", esp_err_to_name(ret));
        if (oled) {
            ssd1306_clear(oled);
            ssd1306_draw_string(oled, 0, 0, "BH1750 INIT FAIL");
            ssd1306_update(oled);
        }
        /* Não aborta a task — o LoRa pode ainda funcionar para debug.
           A leitura simplesmente retornará 0 quando solicitada. */
    }

    /* ── Inicialização do rádio LoRa ─────────────────────────────── */
    ret = sx1262_init(&radio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar SX1262");
        if (oled) {
            ssd1306_clear(oled);
            ssd1306_draw_string(oled, 0, 0, "SX1262 INIT FAIL");
            ssd1306_update(oled);
        }
        vTaskDelete(NULL);   /* Sem rádio não há projeto — encerra task */
        return;
    }

    ESP_LOGI(TAG, "=== LoRa Sensor Node (BH1750) ===");
    ESP_LOGI(TAG, "Aguardando requisicoes do gateway...");

    if (oled) {
        ssd1306_clear(oled);
        ssd1306_draw_string(oled, 0, 0, "LoRa BH1750 Node");
        ssd1306_draw_string(oled, 2, 0, "Aguardando...");
        ssd1306_update(oled);
    }

    /* ── Loop principal: aguarda request → mede → responde ───────── */
    while (1) {

        /* Entra em RX contínuo (timeout=0 = aguarda indefinidamente).
           A função bloqueia até chegar um pacote ou erro de hardware. */
        ret = sx1262_receive_packet(&radio, 0);
        if (ret == ESP_ERR_TIMEOUT) {
            /* Em modo contínuo isso não deveria ocorrer; defesa apenas */
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Erro RX: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Lê o pacote do FIFO do SX1262 e captura RSSI para diagnóstico */
        uint8_t rx_data[LORA_MAX_PAYLOAD];
        uint8_t rx_len = 0;
        int16_t rssi   = 0;

        ret = sx1262_read_packet(&radio, rx_data, &rx_len, &rssi);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Erro ao ler pacote: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "Pacote recebido (%d bytes, RSSI: %d dBm)", rx_len, rssi);

        /* ── Validação do request ────────────────────────────────── */
        /* Descarta silenciosamente qualquer coisa que não seja o protocolo
           definido — pode ser ruído ou outro device LoRa na mesma freq. */
        if (rx_len != REQUEST_LEN) {
            ESP_LOGW(TAG, "Tamanho invalido: %d (esperado %d)", rx_len, REQUEST_LEN);
            continue;
        }
        if (rx_data[0] != HEADER_REQUEST || rx_data[1] != CMD_READ_SENSOR) {
            ESP_LOGW(TAG, "Request invalido: [0x%02X 0x%02X]", rx_data[0], rx_data[1]);
            continue;
        }

        ESP_LOGI(TAG, "Request valido! Lendo BH1750...");

        /* ── Leitura do BH1750 ───────────────────────────────────── */
        /* Bloqueia ~180ms (modo One-Time H-Res). Após retornar, o sensor
           já está em power-down — economia de corrente entre requests. */
        uint16_t lux = 0;
        ret = bh1750_read_lux(&lux);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha leitura BH1750: %s", esp_err_to_name(ret));
            /* Continua e envia 0 ao invés de "engolir" o request — assim
               o gateway ainda recebe resposta e pode flagrar valor zero. */
        }

        /* ── Monta pacote de resposta [0xAA, HIGH, LOW, XOR] ─────── */
        /* XOR serve como checksum
           leve — captura erros de byte único na transmissão LoRa. */
        uint8_t data_high = (uint8_t)(lux >> 8);
        uint8_t data_low  = (uint8_t)(lux & 0xFF);
        uint8_t checksum  = data_high ^ data_low;

        uint8_t response[RESPONSE_LEN] = {
            HEADER_RESPONSE,
            data_high,
            data_low,
            checksum,
        };

        /* Pequeno delay garantindo que o gateway concluiu a transição
           TX→RX antes de enviarmos. Sem isso, o primeiro byte pode ser
           perdido se o gateway ainda estiver em modo TX. */
        vTaskDelay(pdMS_TO_TICKS(50));

        ret = sx1262_send_packet(&radio, response, RESPONSE_LEN);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao enviar resposta: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "Resposta enviada: lux=%u [0x%02X 0x%02X 0x%02X 0x%02X]",
                 lux, response[0], response[1], response[2], response[3]);

        /* ── Atualiza display com leitura mais recente ───────────── */
        if (oled) {
            ssd1306_clear(oled);
            ssd1306_draw_string(oled, 0, 0, "LoRa BH1750 Node");

            snprintf(line, sizeof(line), "Lux: %u", lux);
            ssd1306_draw_string(oled, 2, 0, line);

            snprintf(line, sizeof(line), "RSSI: %d dBm", rssi);
            ssd1306_draw_string(oled, 4, 0, line);

            ssd1306_draw_string(oled, 6, 0, "Enviado OK");
            ssd1306_update(oled);
        }
    }
}

/**
 * Entry point. Inicializa OLED e cria a task principal.
 *
 * O OLED é inicializado AQUI (não dentro da task) porque o handle é
 * passado por argumento e usado em múltiplos pontos do loop.
 */
void app_main(void)
{
    ssd1306_t *oled = NULL;
    esp_err_t  ret  = ssd1306_init(&oled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED init falhou (%s) — continuando sem display",
                 esp_err_to_name(ret));
        /* Display é opcional para diagnóstico; o nó funciona sem ele */
    }

    /* Stack de 4096 bytes é suficiente para o loop + buffers SX1262.
       Pin no Core 1: deixa o Core 0 livre para WiFi/BT (se ativados). */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, oled, 5, NULL, 1);
}
