# Nó Sensor LoRa com BH1750

Este é um dos dois firmwares do projeto da disciplina, o **nó sensor**. O outro lado é o gateway, que vive no projeto irmão `digital_communication_lab_request`.

A ideia é simples: o gateway pede uma leitura, e este nó responde com a luminosidade ambiente medida em lux. Toda a comunicação acontece por LoRa.

## O que esse projeto faz

Quando o nó é ligado, ele inicializa o rádio LoRa, o display OLED e o sensor de luminosidade BH1750, e fica em modo de escuta esperando uma requisição. Assim que recebe o pacote `0xBB 0x01` do gateway, dispara uma medição nova no BH1750, espera a conversão (cerca de 180 ms), e devolve a resposta com o valor de lux empacotado em quatro bytes — `0xAA`, byte alto, byte baixo, e um XOR de checksum. Em paralelo, mostra a leitura no OLED junto com o RSSI do pacote recebido, pra você ter um sinal visual de que tudo deu certo.

## Hardware

A placa é uma Heltec WiFi LoRa 32 V3 — um ESP32-S3 com o rádio SX1262 e o OLED já integrados na PCB. O BH1750 é um módulo externo, ligado por I²C nos pinos 47 (SDA) e 48 (SCL), com o pino ADDR aterrado, o que dá pra ele o endereço I²C `0x23`.

Uma decisão importante de hardware foi colocar o BH1750 num barramento I²C separado do OLED. O ESP32-S3 tem dois controladores I²C em hardware, e usar os dois faz diferença real aqui: a leitura do BH1750 bloqueia o barramento por 180 ms enquanto a conversão acontece, e se o OLED estivesse no mesmo bus, ele ficaria travado esse tempo todo. Com buses separados, o display pode ser atualizado em paralelo sem disputar fila com o sensor.

## Protocolo

A conversa entre o gateway e este nó é bem direta. O gateway manda dois bytes: `0xBB`, que é o cabeçalho do request, e `0x01`, que é o comando "lê o sensor". A resposta tem quatro bytes: `0xAA` como cabeçalho, depois o byte alto e o byte baixo do valor de lux, e por fim um XOR dos dois bytes de dado servindo de checksum.

```
Request:  [0xBB][0x01]
Response: [0xAA][HIGH][LOW][XOR]
```

O XOR sozinho não é uma proteção forte contra corrupção, mas é uma camada extra barata sobre o CRC que o próprio LoRa já faz na camada física. Se algum bit virar no caminho, na maioria dos casos um dos dois mecanismos pega.

Detalhe interessante: o protocolo não diz nada sobre que sensor está do outro lado. Cabe qualquer valor de 16 bits. Isso quer dizer que, quando comecei o projeto com um LDR e depois troquei pelo BH1750, o gateway não precisou ser tocado em nada — ele continua recebendo dois bytes de dado e mostrando.

## Por que BH1750 e por que esse modo de operação

Comecei usando um LDR no ADC só pra validar o protocolo LoRa, mas LDR não entrega lux. Ele entrega uma resistência que vira uma tensão num divisor, que vira um número de 0 a 4095 no ADC. Pra transformar isso em lux de verdade você precisaria caracterizar a curva fotossensível da peça específica. O BH1750 já faz isso por dentro — ele entrega lux direto, com 1 lx de resolução, num range de 0 a mais de 50 mil. A única conta que sobra é dividir o valor cru por 1.2, que é uma constante fixa do datasheet.

Sobre o modo de medição, o BH1750 oferece um modo contínuo (em que ele fica medindo o tempo todo) e um modo "One-Time" (em que ele faz uma medição, entrega o valor, e entra em power-down). Optei pelo One-Time. A razão é que esse nó só responde quando recebe um request — não tem motivo pra ele ficar medindo o tempo todo. No One-Time, cada request dispara uma medição nova, então você nunca recebe um valor velho que tinha ficado guardado no registrador. Como bônus, o sensor passa a maior parte do tempo dormindo, o que é boa hygiene de driver mesmo nesse projeto onde a alimentação vem do USB e energia não é problema.

## Como o código está organizado

```
digital_communication_lab/
├── components/
│   ├── bh1750/        ← driver do sensor (novo)
│   ├── ssd1306/       ← driver do OLED
│   └── sx1262/        ← driver do rádio LoRa
├── main/
│   └── main.c         ← lógica principal
└── CMakeLists.txt
```

O `app_main` faz o setup básico do OLED e cria uma task pinada no Core 1. O Core 0 fica reservado pra stack de WiFi/BT do ESP32, caso ela seja ligada futuramente — separar os cores evita que o LoRa concorra com a stack de rádio do sistema.

A `sensor_task` é toda event-driven: ela bloqueia em `sx1262_receive_packet` esperando o request chegar, e quando chega, faz a leitura, monta a resposta, transmite, atualiza o display, e volta a esperar. Não tem polling, não tem loop ocupado.

O componente `bh1750` é minimalista: dois arquivos, duas funções públicas (`bh1750_init` e `bh1750_read_lux`), wrapping fino em cima do driver I²C master do ESP-IDF. Toda a lógica de timing e dos opcodes do sensor fica encapsulada lá dentro.

## Como rodar

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM_PORT flash monitor
```

Se tudo subiu certo, o log vai mostrar algo assim:

```
I bh1750: BH1750 inicializado em SDA=47 SCL=48 addr=0x23
I sensor: === LoRa Sensor Node (BH1750) ===
I sensor: Aguardando requisicoes do gateway...
```

E o OLED vai exibir "LoRa BH1750 Node — Aguardando...". A partir daí, qualquer pacote `BB 01` que chegar em 915 MHz dispara uma resposta.

## Estado atual

O nó sensor está completo e funcional: drivers prontos, protocolo definido, BH1750 integrado. O próximo passo do projeto é do lado do gateway — fazer ele encaminhar via UART o dado recebido para o FPGA, onde um programa em C- rodando sobre o SO preemptivo vai ler do controlador UART em Verilog que já está pronto.
