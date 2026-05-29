/*
 * ESP32-C6 Zigbee WS2815 Dimmer
 *
 * Zigbee HA Dimmable Light endpoint for Zigbee2MQTT/Home Assistant.
 * Drives a 12 V WS2815 addressable LED strip using SPI DMA to support 300 LEDs!
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "led_strip.h"
#include "led_strip_spi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "platform/esp_zigbee_platform.h"
#include "sdkconfig.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

#define DIMMER_ENDPOINT                 10
#define WS2815_DATA_GPIO                ((gpio_num_t)CONFIG_DIMMER_WS2815_DATA_GPIO)
#define WS2815_LED_COUNT                CONFIG_DIMMER_WS2815_LED_COUNT
#define BOOT_BUTTON_GPIO                9

#define ZIGBEE_CHANNEL_MASK             ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define INSTALL_CODE_POLICY_ENABLE      false
#define MAX_ROUTER_CHILDREN             10
#define NVS_SAVE_DEBOUNCE_MS            3000

#define NVS_DIMMER_NAMESPACE            "dimmer"
#define NVS_KEY_ON                      "on"
#define NVS_KEY_LEVEL                   "level"

#define DIMMER_DEFAULT_ON               false
#define DIMMER_DEFAULT_LEVEL            128

static const char *TAG = "ESP32C6_ZB_DIMMER";

static uint8_t s_manufacturer[] = {11, 'N', 'i', 'n', 'j', 'i', 'n', 'h', 'a', 'D', 'e', 'v'};
static uint8_t s_model[] = {18, 'E', 'S', 'P', '3', '2', 'C', '6', '_', 'P', 'W', 'M', '_', 'D', 'i', 'm', 'm', 'e', 'r'};

typedef struct {
    bool on;
    uint8_t level;
} dimmer_state_t;

static led_strip_handle_t s_strip;
static dimmer_state_t s_state = {
    .on = DIMMER_DEFAULT_ON,
    .level = DIMMER_DEFAULT_LEVEL,
};

static uint8_t s_current_brightness = 0;
static TaskHandle_t s_fade_task_handle = NULL;
static TimerHandle_t s_nvs_save_timer = NULL;

// Curva Gamma para deixar o fade suave ao olho humano
static const uint8_t gamma28[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,
    2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,
    5,   5,   5,   5,   6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,
    9,   9,   10,  10,  10,  11,  11,  12,  12,  12,  13,  13,  14,  14,  15,
    15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  23,
    23,  24,  25,  25,  26,  27,  27,  28,  29,  29,  30,  31,  31,  32,  33,
    34,  34,  35,  36,  37,  38,  38,  39,  40,  41,  42,  42,  43,  44,  45,
    46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  68,  69,  70,  71,  72,  74,  75,  76,  77,
    79,  80,  81,  83,  84,  85,  87,  88,  89,  91,  92,  94,  95,  97,  98,
    100, 101, 103, 104, 106, 107, 109, 111, 112, 114, 116, 117, 119, 121, 122,
    124, 126, 128, 130, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149, 151,
    153, 155, 157, 159, 161, 163, 165, 167, 169, 172, 174, 176, 178, 180, 183,
    185, 187, 190, 192, 194, 197, 199, 202, 204, 207, 209, 212, 214, 217, 219,
    222, 225, 227, 230, 233, 235, 238, 241, 244, 246, 249, 252, 255};


static esp_err_t dimmer_state_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_DIMMER_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved dimmer state, using defaults");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to open NVS namespace");

    uint8_t on = s_state.on ? 1 : 0;
    uint8_t level = s_state.level;
    esp_err_t on_err = nvs_get_u8(handle, NVS_KEY_ON, &on);
    esp_err_t level_err = nvs_get_u8(handle, NVS_KEY_LEVEL, &level);
    nvs_close(handle);

    if (on_err != ESP_OK && on_err != ESP_ERR_NVS_NOT_FOUND) {
        return on_err;
    }
    if (level_err != ESP_OK && level_err != ESP_ERR_NVS_NOT_FOUND) {
        return level_err;
    }

    s_state.on = on != 0;
    s_state.level = level;
    s_current_brightness = s_state.on ? s_state.level : 0;
    ESP_LOGI(TAG, "Loaded state from NVS: on=%d level=%u", s_state.on, s_state.level);
    return ESP_OK;
}

static void dimmer_state_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_DIMMER_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for save: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, NVS_KEY_ON, s_state.on ? 1 : 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, NVS_KEY_LEVEL, s_state.level));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(handle));
    nvs_close(handle);
}

static void dimmer_state_save_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    dimmer_state_save();
}

static void dimmer_state_schedule_save(void)
{
    if (s_nvs_save_timer == NULL) {
        dimmer_state_save();
        return;
    }

    if (xTimerReset(s_nvs_save_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to reset NVS save timer, saving immediately");
        dimmer_state_save();
    }
}

// Tarefa que controla o Brilho e a Cor da Fita (Roda em Backgroud)
static void fade_task(void *arg) {
    bool is_fading = false;

    while (1) {
        uint8_t target = s_state.on ? s_state.level : 0;

        if (s_current_brightness != target) {
            
            // Ativa o fade suave se estava desligada
            if (!is_fading && (s_current_brightness <= 20 || target == 0)) {
                is_fading = true;
            }

            if (is_fading) {
                if (s_current_brightness < target) s_current_brightness++;
                else s_current_brightness--;
            } else {
                // Troca rápida para ajustes de brilho
                s_current_brightness = target;
            }

            uint8_t gamma_val = gamma28[s_current_brightness];

            // Limitador de energia (85% para proteger a fonte)
            gamma_val = (gamma_val * 216) / 255;

            // Cor Âmbar: R:255, G:222, B:33
            uint32_t temp_r = (gamma_val * 255) / 255;
            uint32_t temp_g = (gamma_val * 222) / 255;
            uint32_t temp_b = (gamma_val * 33) / 255;
            
            uint8_t final_r = (uint8_t)temp_r;
            uint8_t final_g = (uint8_t)temp_g;
            uint8_t final_b = (uint8_t)temp_b;

            for (uint32_t i = 0; i < WS2815_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, final_r, final_g, final_b);
            }
            led_strip_refresh(s_strip);

            if (is_fading) {
                vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
            }

            if (s_current_brightness == target) {
                is_fading = false;
            }
        } else {
            is_fading = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

static esp_err_t ws2815_init(void)
{
    gpio_reset_pin(WS2815_DATA_GPIO);
    gpio_set_direction(WS2815_DATA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WS2815_DATA_GPIO, 0);

    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2815_DATA_GPIO,
        .max_leds = WS2815_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    // A MÁGICA: Usar SPI com DMA ativado (Resolve o problema de travar fita de 300 LEDs no ESP32-C6)
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags = {
            .with_dma = true, 
        },
    };

    ESP_RETURN_ON_ERROR(led_strip_new_spi_device(&strip_config, &spi_config, &s_strip), TAG, "Failed to create LED strip");
    ESP_LOGI(TAG, "WS2815 configured on GPIO%d, leds=%d with SPI DMA", WS2815_DATA_GPIO, WS2815_LED_COUNT);
    return ESP_OK;
}

static void zigbee_update_attribute(uint16_t cluster_id, uint16_t attr_id, void *value)
{
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Could not acquire Zigbee lock to update attribute");
        return;
    }

    esp_zb_zcl_set_attribute_val(DIMMER_ENDPOINT, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, value, false);
    esp_zb_lock_release();
}

static void dimmer_set_on(bool on, bool update_zigbee)
{
    if (s_state.on == on) {
        return;
    }

    s_state.on = on;
    dimmer_state_schedule_save();
    
    if (s_fade_task_handle) {
        xTaskNotifyGive(s_fade_task_handle);
    }

    if (update_zigbee) {
        zigbee_update_attribute(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &s_state.on);
    }
}

static void dimmer_set_level(uint8_t level, bool update_zigbee)
{
    if (s_state.level == level) {
        return;
    }

    s_state.level = level;
    dimmer_state_schedule_save();
    
    if (s_fade_task_handle) {
        xTaskNotifyGive(s_fade_task_handle);
    }

    if (update_zigbee) {
        zigbee_update_attribute(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                                ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
                                &s_state.level);
    }
}

static esp_zb_cluster_list_t *zigbee_create_dimmable_light_clusters(void)
{
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_groups_cluster_cfg_t groups_cfg = {
        .groups_name_support_id = ESP_ZB_ZCL_GROUPS_NAME_SUPPORT_DEFAULT_VALUE,
    };
    esp_zb_scenes_cluster_cfg_t scenes_cfg = {
        .scenes_count = ESP_ZB_ZCL_SCENES_SCENE_COUNT_DEFAULT_VALUE,
        .current_scene = ESP_ZB_ZCL_SCENES_CURRENT_SCENE_DEFAULT_VALUE,
        .current_group = ESP_ZB_ZCL_SCENES_CURRENT_GROUP_DEFAULT_VALUE,
        .scene_valid = ESP_ZB_ZCL_SCENES_SCENE_VALID_DEFAULT_VALUE,
        .name_support = ESP_ZB_ZCL_SCENES_NAME_SUPPORT_DEFAULT_VALUE,
    };
    esp_zb_on_off_cluster_cfg_t on_off_cfg = {
        .on_off = s_state.on,
    };
    esp_zb_level_cluster_cfg_t level_cfg = {
        .current_level = s_state.level,
    };

    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model);

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, esp_zb_identify_cluster_create(&identify_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_groups_cluster(cluster_list, esp_zb_groups_cluster_create(&groups_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_scenes_cluster(cluster_list, esp_zb_scenes_cluster_create(&scenes_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_on_off_cluster(cluster_list, esp_zb_on_off_cluster_create(&on_off_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_level_cluster(cluster_list, esp_zb_level_cluster_create(&level_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    return cluster_list;
}

static esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id != ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return ESP_OK;
    }

    const esp_zb_zcl_set_attr_value_message_t *msg = (const esp_zb_zcl_set_attr_value_message_t *)message;
    if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS || msg->info.dst_endpoint != DIMMER_ENDPOINT) {
        return ESP_OK;
    }

    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        msg->attribute.data.value != NULL) {
        const bool on = *(bool *)msg->attribute.data.value;
        ESP_LOGI(TAG, "Zigbee set on/off: %d", on);
        dimmer_set_on(on, false);
    } else if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
               msg->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID &&
               msg->attribute.data.value != NULL) {
        const uint8_t level = *(uint8_t *)msg->attribute.data.value;
        ESP_LOGI(TAG, "Zigbee set brightness: %u", level);
        dimmer_set_level(level, false);
    }

    return ESP_OK;
}

static void zigbee_start_network_steering(uint8_t param)
{
    (void)param;
    ESP_LOGI(TAG, "Starting Zigbee network steering");
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_s)
{
    const esp_zb_app_signal_type_t signal = *signal_s->p_app_signal;
    const esp_err_t status = signal_s->esp_err_status;

    ESP_LOGI(TAG, "Zigbee signal: %s, status=%s", esp_zb_zdo_signal_to_string(signal), esp_err_to_name(status));

    switch (signal) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        if (status == ESP_OK) {
            zigbee_start_network_steering(0);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Joined Zigbee network");
        } else {
            esp_zb_scheduler_alarm(zigbee_start_network_steering, 0, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee commissioning completed");
        } else {
            ESP_LOGW(TAG, "Zigbee commissioning failed, retrying in 5 s");
            esp_zb_scheduler_alarm(zigbee_start_network_steering, 0, 5000);
        }
        break;

    default:
        break;
    }
}

static void zigbee_task(void *pvParameters)
{
    (void)pvParameters;

    esp_zb_platform_config_t platform_config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_config));

    esp_zb_cfg_t zigbee_config = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = INSTALL_CODE_POLICY_ENABLE,
        .nwk_cfg = {
            .zczr_cfg = {
                .max_children = MAX_ROUTER_CHILDREN,
            },
        },
    };
    esp_zb_init(&zigbee_config);
    esp_zb_set_primary_network_channel_set(ZIGBEE_CHANNEL_MASK);

    esp_zb_ep_list_t *endpoint_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = DIMMER_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(endpoint_list, zigbee_create_dimmable_light_clusters(), endpoint_config);

    esp_zb_device_register(endpoint_list);
    esp_zb_core_action_handler_register(zigbee_action_handler);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// Tarefa do Botão Físico (Factory Reset e Controle Local)
static void button_task(void *arg) {
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(BOOT_BUTTON_GPIO);

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                uint32_t press_time = 0;
                bool factory_reset = false;

                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    press_time += 100;
                    if (press_time >= 3000) {
                        ESP_LOGW(TAG, "Factory Reset acionado pelo botao fisico!");
                        
                        nvs_flash_erase();
                        nvs_flash_init();
                        
                        esp_zb_factory_reset();
                        
                        while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                        factory_reset = true;
                        break;
                    }
                }

                if (!factory_reset) {
                    ESP_LOGI(TAG, "Botao fisico pressionado - Alternando a luz");
                    dimmer_set_on(!s_state.on, true);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(dimmer_state_load());
    s_nvs_save_timer = xTimerCreate("nvs_save",
                                    pdMS_TO_TICKS(NVS_SAVE_DEBOUNCE_MS),
                                    pdFALSE,
                                    NULL,
                                    dimmer_state_save_timer_cb);
    if (s_nvs_save_timer == NULL) {
        ESP_LOGW(TAG, "NVS save debounce timer unavailable, state will be saved immediately");
    }
    ESP_ERROR_CHECK(ws2815_init());

    // Inicia a tarefa responsável pelo fade suave em segundo plano
    xTaskCreate(fade_task, "fade_task", 4096, NULL, 4, &s_fade_task_handle);

    // Inicia a tarefa do Botão Físico
    xTaskCreate(button_task, "button_task", 4096, NULL, 4, NULL);

    // Como é o boot inicial, pede para a task aplicar a cor instantaneamente
    if (s_fade_task_handle) {
        xTaskNotifyGive(s_fade_task_handle);
    }

    ESP_LOGI(TAG, "Starting ESP32-C6 Zigbee dimmer, manufacturer=NinjinhaDev model=ESP32C6_PWM_Dimmer");
    xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL);
}
