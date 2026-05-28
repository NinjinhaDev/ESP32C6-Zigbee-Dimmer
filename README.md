# ESP32-C6 Zigbee WS2815 Dimmer

Firmware ESP-IDF para usar um ESP32-C6 como dimmer Zigbee inteligente para fita WS2815 12 V.

O dispositivo entra na rede Zigbee como uma luz dimerizável simples, com suporte a `on/off` e `brightness`, perfeitamente compatível com Zigbee2MQTT, Home Assistant e Alexa.

## Novos Recursos Avançados (v2.0)

- **Fade Suave Cinematográfico:** Transições suaves ao ligar/desligar com proteção contra saltos bruscos enviados por hubs (Home Assistant/Tuya).
- **Cor Âmbar Fixa:** A fita acende com a cor pré-definida `RGB(255, 222, 33)` injetada diretamente no sinal RMT, mantendo a simplicidade de controle de um dimmer branco comum no app.
- **Proteção de Memória (NVS Debounce):** O estado só é salvo fisicamente na memória flash 3 segundos após a última alteração do usuário, evitando o desgaste prematuro da NVS.
- **Limitador de Potência:** Para proteger fontes subdimensionadas, o firmware impõe um teto absoluto de 85% do brilho máximo.
- **Correção Gamma 2.8:** O mapa de brilho é filtrado matematicamente para que o comportamento do LED se alinhe à percepção logarítmica do olho humano.
- **Reset de Fábrica Seguro:** Segurar o botão físico (GPIO 9) por 3 segundos aciona a formatação da NVS e do Zigbee, com trava de segurança para evitar que a placa entre em "Download Mode" acidentalmente.
- **Menuconfig Nativo:** Configurável facilmente pelas ferramentas de build da Espressif sem precisar alterar código.

## Hardware

Este projeto foi feito para fita **WS2815 12 V** com fonte externa.

Ligação básica:

```text
Fonte 12 V +  -> +12V da fita
Fonte 12 V -  -> GND da fita
Fonte 12 V -  -> GND do ESP32-C6
GPIO4 ESP32-C6 -> resistor 330-470 ohms -> DIN da fita
Botão Tátil    -> Entre GPIO 9 e GND
```

Recomendado:
- Capacitor de 1000 uF ou maior entre `+12V` e `GND` perto da fita.
- Level shifter 3.3 V para 5 V no sinal de dados.
- Cabo curto entre ESP32-C6 e entrada de dados da fita.

> Nunca ligue 12 V em nenhum pino do ESP32-C6! O ESP apenas envia o sinal de dados de 3.3V e compartilha o GND com a fonte da fita.

## Configuração

Você não precisa mais alterar código fonte para ajustar o pino ou a quantidade de LEDs.
Utilize o menu do ESP-IDF:

```bash
idf.py menuconfig
```
Navegue até **"Zigbee Dimmer Configuration"** e altere:
- O pino de dados do LED (Padrão: GPIO 4)
- A quantidade de LEDs da fita (Padrão: 300)

## Requisitos

- ESP-IDF v6.0.1.
- Target `esp32c6`.
- Python e toolchain instalados pelo ESP-IDF.

Os componentes externos são declarados em `main/idf_component.yml`:
- `espressif/esp-zigbee-lib`
- `espressif/esp-zboss-lib`
- `espressif/led_strip`

Ao compilar, o ESP-IDF baixa os componentes automaticamente.

## Build e Flash

No terminal ESP-IDF:

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

Se a tabela de partições mudar, ou se estiver vindo de outro projeto diferente, use:

```bash
idf.py -p COMx erase-flash flash monitor
```

## Pareamento Zigbee (Home Assistant / Z2M)

1. Ative o modo "Permit Join" no seu Hub (Zigbee2MQTT, ZHA, SmartLife).
2. Se o dispositivo já estiver na rede e você quiser reparear: **Segure o botão físico (GPIO 9) por 3 segundos**, solte e aguarde a placa reiniciar.
3. O dispositivo irá piscar buscando pareamento.

Ele deve aparecer na rede como:
- Fabricante: `NinjinhaDev`
- Modelo: `ESP32C6_PWM_Dimmer`

Aproveite o seu Dimmer inteligente super otimizado!
