# Digital Communication Lab

Projeto de comunicacao digital utilizando LoRa (SX1262) com ESP32-S3, desenvolvido sobre a plataforma Heltec WiFi LoRa 32 V3.

## Hardware

- **Placa:** Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
- **Display:** SSD1306 OLED 128x64 (I2C, endereco 0x3C)
- **Sensor:** LDR conectado ao GPIO1 (ADC1_CH0) com divisor resistivo
- **Radio:** Semtech SX1262 via SPI

### Pinagem (Heltec V3)

| Funcao         | GPIO |
|----------------|------|
| SPI MOSI       | 10   |
| SPI MISO       | 11   |
| SPI SCK        | 9    |
| SX1262 CS      | 8    |
| SX1262 RST     | 12   |
| SX1262 BUSY    | 13   |
| SX1262 DIO1    | 14   |
| OLED SDA       | 17   |
| OLED SCL       | 18   |
| OLED RST       | 21   |
| OLED Vext Ctrl | 36   |
| LDR (ADC)      | 1    |

## Arquitetura

```
digital_communication_lab/
├── main/
│   └── main.c              # Aplicacao principal (sensor node)
├── components/
│   ├── sx1262/              # Driver LoRa SX1262 (SPI)
│   │   ├── sx1262.c
│   │   └── include/sx1262.h
│   └── ssd1306/             # Driver OLED SSD1306 (I2C master)
│       ├── ssd1306.c
│       └── include/ssd1306.h
├── CMakeLists.txt
└── README.md
```

## Protocolo LoRa

O sistema usa um protocolo request/response simples entre gateway e sensor node:

### Request (gateway -> sensor)

| Byte | Valor | Descricao           |
|------|-------|---------------------|
| 0    | 0xBB  | Header de request   |
| 1    | 0x01  | Comando: ler sensor |

### Response (sensor -> gateway)

| Byte | Valor       | Descricao                |
|------|-------------|--------------------------|
| 0    | 0xAA        | Header de response       |
| 1    | DATA_HIGH   | Byte alto do ADC (12-bit)|
| 2    | DATA_LOW    | Byte baixo do ADC        |
| 3    | XOR         | Checksum (byte1 ^ byte2) |

## Parametros LoRa

| Parametro   | Valor     |
|-------------|-----------|
| Frequencia  | 915 MHz   |
| SF          | 7         |
| Bandwidth   | 125 kHz   |
| Coding Rate | 4/5       |
| Preambulo   | 8 simbolos|
| Potencia TX | +22 dBm   |

## Funcionamento

1. O **sensor node** inicializa o display OLED, o ADC (LDR) e o radio SX1262.
2. Entra em modo RX continuo aguardando requisicoes.
3. Ao receber `[0xBB, 0x01]` do gateway:
   - Le o valor do LDR via ADC (0-4095).
   - Monta o pacote de resposta com header, dados e checksum XOR.
   - Transmite a resposta via LoRa.
   - Atualiza o display OLED com o valor do LDR e RSSI do pacote recebido.

## Build

### Pre-requisitos

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/)
- Heltec WiFi LoRa 32 V3 conectada via USB

### Compilar e gravar

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

## Componentes

### sx1262

Driver minimo para o radio LoRa SX1262 via SPI. Suporta:
- Inicializacao completa (TCXO, PA, modulacao)
- Envio de pacotes (bloqueante)
- Recepcao de pacotes (bloqueante ou com timeout)
- Leitura de RSSI

### ssd1306

Driver para o display OLED SSD1306 128x64 via I2C (nova API `i2c_master.h` do ESP-IDF v6.0). Suporta:
- Inicializacao com controle de Vext e reset por hardware
- Framebuffer em RAM com flush por pagina
- Renderizacao de texto com fonte 6x8
