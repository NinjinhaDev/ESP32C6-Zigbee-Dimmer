# ESP32-C6 Zigbee WS2815 Dimmer

Firmware ESP-IDF para usar um ESP32-C6 como dimmer Zigbee para fita WS2815 12 V.

O dispositivo entra na rede Zigbee como uma luz dimerizavel simples, com suporte a `on/off` e `brightness`, compativel com Zigbee2MQTT e Home Assistant.

## Recursos

- ESP32-C6 com radio IEEE 802.15.4 nativo.
- Endpoint Zigbee Home Automation Dimmable Light.
- Controle `on/off`.
- Controle de brilho.
- Saida de dados WS2815 via RMT usando o componente `led_strip`.
- Estado `on/off` e brilho salvos em NVS.
- Inicializacao sem brilho aleatorio.

## Hardware

Este projeto foi feito para fita **WS2815 12 V** com fonte externa.

Ligacao basica:

```text
Fonte 12 V +  -> +12V da fita
Fonte 12 V -  -> GND da fita
Fonte 12 V -  -> GND do ESP32-C6
GPIO4 ESP32-C6 -> resistor 330-470 ohms -> DIN da fita
```

Recomendado:

- Capacitor de 1000 uF ou maior entre `+12V` e `GND` perto da fita.
- Level shifter 3.3 V para 5 V no sinal de dados.
- Cabo curto entre ESP32-C6 e entrada de dados da fita.

Nunca ligue 12 V em GPIO do ESP32-C6. O ESP32-C6 apenas envia o sinal de dados e compartilha o GND com a fonte da fita.

## Configuracao principal

No arquivo `main/main.c`:

```c
#define WS2815_DATA_GPIO                GPIO_NUM_4
#define WS2815_LED_COUNT                60
```

Altere `WS2815_LED_COUNT` para a quantidade real de LEDs da sua fita.

Se usar outro pino de dados, altere `WS2815_DATA_GPIO`.

## Requisitos

- ESP-IDF v6.0.1.
- Target `esp32c6`.
- Python e toolchain instalados pelo ESP-IDF.
- Opcional: VS Code com a extensao oficial da Espressif.

Os componentes externos sao declarados em `main/idf_component.yml`:

- `espressif/esp-zigbee-lib`
- `espressif/esp-zboss-lib`
- `espressif/led_strip`

Ao compilar, o ESP-IDF baixa os componentes automaticamente.

## Build

No terminal ESP-IDF:

```bash
idf.py set-target esp32c6
idf.py build
```

No Windows, se estiver usando a instalacao padrao da Espressif:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'; idf.py set-target esp32c6; idf.py build"
```

## Flash

Troque `COMx` pela porta serial do ESP32-C6:

```bash
idf.py -p COMx flash monitor
```

Se a tabela de particoes mudar, ou se o dispositivo ja tiver outro firmware Zigbee, use:

```bash
idf.py -p COMx erase-flash flash monitor
```

## Zigbee2MQTT

1. Abra o Zigbee2MQTT.
2. Ative `Permit join`.
3. Reinicie o ESP32-C6.
4. Aguarde o dispositivo entrar na rede.

Ele deve aparecer como:

```text
NinjinhaDev ESP32C6_PWM_Dimmer
```

Renomeie no Zigbee2MQTT ou Home Assistant para:

```text
ESP32-C6 Zigbee Dimmer
```

O dispositivo usa clusters Zigbee padrao de luz dimerizavel:

- Basic
- Identify
- Groups
- Scenes
- On/Off
- Level Control

Se aparecer como `unsupported` no Zigbee2MQTT, gere uma external definition pelo Dev Console do Zigbee2MQTT. Como o firmware usa clusters padrao, a exposicao esperada e uma luz com `state` e `brightness`.

## Arquivos importantes

```text
CMakeLists.txt
main/CMakeLists.txt
main/idf_component.yml
main/main.c
partitions.csv
sdkconfig.defaults
README.md
```

Arquivos como `build/`, `managed_components/`, `sdkconfig` e `.vscode/` sao gerados localmente e nao precisam ser versionados.
