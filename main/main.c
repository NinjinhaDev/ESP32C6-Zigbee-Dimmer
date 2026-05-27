/*
 * ESP32-C6 Zigbee WS2815 Dimmer
 *
 * Zigbee HA Dimmable Light endpoint for Zigbee2MQTT/Home Assistant.
 * Drives a 12 V WS2815 addressable LED strip through one data GPIO using RMT.
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
#include "ha/esp_zigbee_ha_standard.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "platform/esp_zigbee_platform.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

#define DIMMER_ENDPOINT                 10
#define WS2815_DATA_GPIO                GPIO_NUM_4
#define WS2815_LED_COUNT                60
#define WS2815_RMT_RESOLUTION_HZ        (10 * 1000 * 1000)

#define ZIGBEE_CHANNEL_MASK             ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define INSTALL_CODE_POLICY_ENABLE      false
#define MAX_ROUTER_CHILDREN             10

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

static esp_err_t ws2815_apply_state(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t value = s_state.on ? s_state.level : 0;
    for (uint32_t i = 0; i < WS2815_LED_COUNT; i++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, i, value, value, value), TAG, "Failed to set LED pixel");
    }
    ESP_RETURN_ON_ERROR(led_strip_refresh(s_strip), TAG, "Failed to refresh LED strip");
    ESP_LOGI(TAG, "Applied strip state: on=%d level=%u rgb=%u", s_state.on, s_state.level, value);
    return ESP_OK;
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

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2815_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "Failed to create LED strip");
    ESP_RETURN_ON_ERROR(ws2815_apply_state(), TAG, "Failed to apply initial state");
    ESP_LOGI(TAG, "WS2815 configured on GPIO%d, leds=%d", WS2815_DATA_GPIO, WS2815_LED_COUNT);
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ws2815_apply_state());
    dimmer_state_save();

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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ws2815_apply_state());
    dimmer_state_save();

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

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(dimmer_state_load());
    ESP_ERROR_CHECK(ws2815_init());

    ESP_LOGI(TAG, "Starting ESP32-C6 Zigbee dimmer, manufacturer=NinjinhaDev model=ESP32C6_PWM_Dimmer");
    xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL);
}
