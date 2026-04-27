/**
 * @file  bh1750.h
 * @brief Driver para sensor de luminosidade BH1750FVI (Rohm)
 *
 * O BH1750 é um sensor I²C de luminosidade ambiente com saída direta
 * em lux (sem necessidade de calibração ou cálculos de fotocélula como
 * no LDR). Usa a fórmula: lux = raw_value / 1.2
 *
 * MODO USADO: One-Time High-Resolution Mode (0x20)
 *   - Cada medição: ~120ms (típico) / 180ms (máx)
 *   - Resolução: 1 lx
 *   - Range: 1 ~ 65535 lx
 *   - Após a leitura, o sensor entra automaticamente em power-down
 *
 * Por que One-Time?
 *   - Este nó só responde quando recebe request LoRa do gateway
 *   - Modo contínuo manteria o sensor ativo desnecessariamente
 *   - Cada request gera uma medição fresca, sem dados antigos em cache
 *
 * Pinagem (Heltec WiFi LoRa 32 V3 / ESP32-S3):
 *   - SDA: GPIO 47 (I2C_NUM_1, barramento separado do OLED)
 *   - SCL: GPIO 48
 *   - VCC: 3.3V
 *   - GND: GND
 *   - ADDR: GND (endereço I²C = 0x23)
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Inicializa o driver BH1750.
 *
 * Configura o barramento I2C_NUM_1 (separado do OLED), faz o reset
 * do sensor, e o coloca em power-on.
 *
 * @return ESP_OK em sucesso, ou erro do ESP-IDF.
 */
esp_err_t bh1750_init(void);

/**
 * @brief Lê a luminosidade ambiente em lux.
 *
 * Bloqueia por ~120-180ms enquanto o sensor faz a conversão.
 * Após a leitura, o sensor entra em power-down automaticamente
 * (característica do modo One-Time).
 *
 * @param[out] lux_out  Ponteiro para uint16_t onde o lux será escrito.
 *                      Range: 0 ~ 54612 (limitado pelo divisor 1.2).
 * @return ESP_OK em sucesso, ou erro do ESP-IDF.
 */
esp_err_t bh1750_read_lux(uint16_t *lux_out);
