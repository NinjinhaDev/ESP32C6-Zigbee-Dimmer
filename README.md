# ESP32-C6 Zigbee WS2815 Dimmer

Firmware ESP-IDF para transformar um ESP32-C6 em um dimmer Zigbee para fita LED endereçável WS2815 12 V.

O dispositivo entra na rede como uma luz dimerizável Zigbee, com suporte a `on/off` e `brightness`, compatível com Zigbee2MQTT, Home Assistant, ZHA e integrações que reconhecem o perfil Zigbee Home Automation.

## Status

- Versão atual: v2.0
- Placa alvo: ESP32-C6
- Fita alvo: WS2815 12 V
- Controle Zigbee: endpoint de luz dimerizável
- Saída LED: SPI DMA via componente `led_strip`
- Cor fixa da fita: âmbar `RGB(255, 222, 33)`

## Recursos

- Fade suave ao ligar e desligar.
- Ajuste rápido de brilho quando a fita já está acesa.
- Correção gamma 2.8 para resposta visual mais natural.
- Limitador de potência em 85% do brilho máximo.
- Estado salvo em NVS com debounce de 3 segundos para reduzir gravações na flash.
- Botão físico no GPIO 9 para alternar liga/desliga.
- Reset de fábrica segurando o botão por 3 segundos.
- Configuração de GPIO e quantidade de LEDs via `idf.py menuconfig`.

## Hardware

Este projeto foi feito para fita **WS2815 12 V** com fonte externa. O ESP32-C6 não alimenta a fita; ele apenas envia o sinal de dados e compartilha o GND com a fonte.

Ligação básica:

```text
Fonte 12 V +       -> +12V da fita
Fonte 12 V -       -> GND da fita
Fonte 12 V -       -> GND do ESP32-C6
GPIO4 ESP32-C6     -> resistor 330-470 ohms -> DIN da fita
Botão físico       -> entre GPIO 9 e GND
```

Recomendado:

- Capacitor de 1000 uF ou maior entre `+12V` e `GND` perto da fita.
- Level shifter 3.3 V para 5 V no sinal de dados, principalmente com cabo longo.
- Cabo curto entre o ESP32-C6 e a entrada de dados da fita.
- Fonte dimensionada para a quantidade real de LEDs instalada.

> Nunca ligue 12 V em nenhum pino do ESP32-C6.

## Configuração

Os padrões atuais são:

| Opção | Padrão |
| --- | --- |
| GPIO de dados WS2815 | GPIO 4 |
| Quantidade de LEDs | 300 |
| Botão físico | GPIO 9 |
| Fabricante Zigbee | `NinjinhaDev` |
| Modelo Zigbee | `ESP32C6_PWM_Dimmer` |

Para alterar o pino de dados ou a quantidade de LEDs:

```bash
idf.py menuconfig
```

Depois navegue até:

```text
Zigbee Dimmer Configuration
```

## Estrutura do projeto

```text
.
+-- CMakeLists.txt
+-- partitions.csv
+-- sdkconfig.defaults
+-- dependencies.lock
+-- main/
|   +-- CMakeLists.txt
|   +-- Kconfig.projbuild
|   +-- idf_component.yml
|   +-- main.c
+-- README.md
```

Arquivos gerados localmente, como `build/`, `managed_components/`, `sdkconfig` e configurações de editor, ficam fora do Git pelo `.gitignore`.

## Requisitos

- ESP-IDF 6.0.x.
- Target `esp32c6`.
- Python e toolchain instalados pelo ESP-IDF.

Componentes externos declarados em `main/idf_component.yml`:

- `espressif/esp-zigbee-lib`
- `espressif/esp-zboss-lib`
- `espressif/led_strip`

O ESP-IDF baixa os componentes automaticamente durante o build.

## Build e flash

No terminal com o ambiente ESP-IDF carregado:

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

Troque `COMx` pela porta serial da placa.

Se estiver migrando de outro firmware, se mudou partições ou se quiser limpar pareamento anterior:

```bash
idf.py -p COMx erase-flash flash monitor
```

## Pareamento Zigbee

1. Ative `Permit Join` no Zigbee2MQTT, ZHA ou hub compatível.
2. Ligue o ESP32-C6 com o firmware gravado.
3. Aguarde o dispositivo entrar na rede Zigbee.

Para remover o pareamento e voltar ao estado de fábrica, segure o botão físico no GPIO 9 por 3 segundos. O firmware apaga NVS/Zigbee e reinicia o processo de pareamento.

O dispositivo deve aparecer como:

- Fabricante: `NinjinhaDev`
- Modelo: `ESP32C6_PWM_Dimmer`

## Notas de operação

- O firmware usa uma luz Zigbee dimerizável simples. A cor âmbar é fixa no firmware, então o app controla apenas liga/desliga e brilho.
- O brilho salvo em NVS é restaurado ao reiniciar.
- A gravação em NVS é atrasada em 3 segundos após mudanças de estado para evitar desgaste por comandos repetidos.
- O limite de 85% ajuda a proteger fontes menores, mas não substitui uma fonte corretamente dimensionada.
