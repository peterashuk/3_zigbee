#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"

#define OLED_SCL_GPIO GPIO_NUM_5
#define OLED_SDA_GPIO GPIO_NUM_4
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_ADDRESS_0 0x3C
#define OLED_ADDRESS_1 0x3D

#define ZIGBEE_ENDPOINT 1
#define ZIGBEE_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define ZIGBEE_ED_KEEP_ALIVE_MS 3000

static const char *TAG = "zigbee_switch";
static i2c_master_dev_handle_t oled_display;
static QueueHandle_t display_state_queue;
static uint8_t framebuffer[OLED_WIDTH * OLED_PAGES];

static const uint8_t font_5x7[][5] = {
	['A' - 'A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
	['B' - 'A'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
	['C' - 'A'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
	['E' - 'A'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
	['F' - 'A'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
	['G' - 'A'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
	['H' - 'A'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
	['I' - 'A'] = {0x00, 0x41, 0x7F, 0x41, 0x00},
	['N' - 'A'] = {0x7F, 0x02, 0x04, 0x08, 0x7F},
	['O' - 'A'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
	['S' - 'A'] = {0x46, 0x49, 0x49, 0x49, 0x31},
	['T' - 'A'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
	['W' - 'A'] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
	['Z' - 'A'] = {0x61, 0x51, 0x49, 0x45, 0x43},
};

static void oled_set_pixel(uint8_t x, uint8_t y)
{
	framebuffer[(y / 8) * OLED_WIDTH + x] |= (uint8_t)(1U << (y % 8));
}

static void oled_draw_text(const char *text, uint8_t x, uint8_t y, uint8_t scale)
{
	for (; *text != '\0'; ++text, x = (uint8_t)(x + 6 * scale)) {
		if (*text == ' ') {
			continue;
		}

		const uint8_t *glyph = font_5x7[*text - 'A'];
		for (uint8_t column = 0; column < 5; ++column) {
			for (uint8_t row = 0; row < 7; ++row) {
				if ((glyph[column] & (1U << row)) == 0) {
					continue;
				}
				for (uint8_t dx = 0; dx < scale; ++dx) {
					for (uint8_t dy = 0; dy < scale; ++dy) {
						oled_set_pixel((uint8_t)(x + column * scale + dx),
									   (uint8_t)(y + row * scale + dy));
					}
				}
			}
		}
	}
}

static void oled_render_state(bool is_on)
{
	memset(framebuffer, 0, sizeof(framebuffer));
	oled_draw_text("ZIGBEE SWITCH", 25, 9, 1);

	const char *state_text = is_on ? "ON" : "OFF";
	uint8_t state_width = (uint8_t)(strlen(state_text) * 24 - 4);
	oled_draw_text(state_text, (uint8_t)((OLED_WIDTH - state_width) / 2), 30, 4);

	const uint8_t set_window[] = {
		0x00, 0x21, 0x00, OLED_WIDTH - 1,
		0x22, 0x00, OLED_PAGES - 1,
	};
	ESP_ERROR_CHECK(i2c_master_transmit(oled_display, set_window, sizeof(set_window), -1));

	uint8_t packet[OLED_WIDTH + 1];
	packet[0] = 0x40;
	for (uint8_t page = 0; page < OLED_PAGES; ++page) {
		memcpy(&packet[1], &framebuffer[page * OLED_WIDTH], OLED_WIDTH);
		ESP_ERROR_CHECK(i2c_master_transmit(oled_display, packet, sizeof(packet), -1));
	}
}

static void oled_state_task(void *arg)
{
	bool is_on;
	for (;;) {
		if (xQueueReceive(display_state_queue, &is_on, portMAX_DELAY) == pdTRUE) {
			oled_render_state(is_on);
		}
	}
}

static void oled_init(void)
{
	i2c_master_bus_config_t bus_config = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = OLED_SDA_GPIO,
		.scl_io_num = OLED_SCL_GPIO,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t bus;
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

	uint8_t address = OLED_ADDRESS_0;
	esp_err_t err = i2c_master_probe(bus, address, 100);
	if (err != ESP_OK) {
		address = OLED_ADDRESS_1;
		err = i2c_master_probe(bus, address, 100);
	}
	ESP_ERROR_CHECK(err);

	i2c_device_config_t device_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = 100000,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &oled_display));

	const uint8_t init_commands[] = {
		0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
		0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA,
		0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4,
		0xA6, 0xAF,
	};
	ESP_ERROR_CHECK(i2c_master_transmit(oled_display, init_commands, sizeof(init_commands), -1));
	ESP_LOGI(TAG, "SSD1306 initialized at 0x%02X", address);
}

static void commissioning_retry(uint8_t mode)
{
	esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to restart Zigbee commissioning: %s", esp_err_to_name(err));
	}
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal)
{
	uint32_t signal_type = *signal->p_app_signal;
	switch (signal_type) {
	case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
		ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION));
		break;
	case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
	case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
		if (signal->esp_err_status == ESP_OK && esp_zb_bdb_is_factory_new()) {
			ESP_LOGI(TAG, "Starting Zigbee network steering");
			ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING));
		} else if (signal->esp_err_status != ESP_OK) {
			ESP_LOGE(TAG, "Zigbee startup failed: %s", esp_err_to_name(signal->esp_err_status));
		}
		break;
	case ESP_ZB_BDB_SIGNAL_STEERING:
		if (signal->esp_err_status == ESP_OK) {
			ESP_LOGI(TAG, "Joined Zigbee network");
		} else {
			ESP_LOGW(TAG, "Network steering failed: %s; retrying", esp_err_to_name(signal->esp_err_status));
			esp_zb_scheduler_alarm(commissioning_retry, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
		}
		break;
	default:
		ESP_LOGI(TAG, "Zigbee signal %lu: %s", (unsigned long)signal_type,
				 esp_err_to_name(signal->esp_err_status));
		break;
	}
}

static esp_err_t zigbee_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
	if (message == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
		ESP_LOGW(TAG, "Zigbee attribute update failed: status %d", message->info.status);
		return ESP_ERR_INVALID_STATE;
	}
	if (message->info.dst_endpoint != ZIGBEE_ENDPOINT ||
		message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF ||
		message->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
		return ESP_OK;
	}
	if (message->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL ||
		message->attribute.data.value == NULL) {
		ESP_LOGE(TAG, "Received malformed Zigbee On/Off attribute");
		return ESP_ERR_INVALID_ARG;
	}

	bool is_on = *(bool *)message->attribute.data.value;
	if (xQueueOverwrite(display_state_queue, &is_on) != pdPASS) {
		ESP_LOGE(TAG, "Failed to queue OLED state update");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "Switch state is %s", is_on ? "ON" : "OFF");
	return ESP_OK;
}

static esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id,
									   const void *message)
{
	if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
		return zigbee_attribute_handler(message);
	}
	return ESP_OK;
}

static void zigbee_task(void *arg)
{
	esp_zb_cfg_t network_config = {
		.esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
		.install_code_policy = false,
		.nwk_cfg.zed_cfg = {
			.ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
			.keep_alive = ZIGBEE_ED_KEEP_ALIVE_MS,
		},
	};
	esp_zb_init(&network_config);

	esp_zb_on_off_light_cfg_t switch_config = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
	esp_zb_cluster_list_t *clusters = esp_zb_on_off_light_clusters_create(&switch_config);
	ESP_ERROR_CHECK(clusters == NULL ? ESP_ERR_NO_MEM : ESP_OK);

	esp_zb_ep_list_t *endpoints = esp_zb_ep_list_create();
	ESP_ERROR_CHECK(endpoints == NULL ? ESP_ERR_NO_MEM : ESP_OK);
	esp_zb_endpoint_config_t endpoint_config = {
		.endpoint = ZIGBEE_ENDPOINT,
		.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
		.app_device_id = ESP_ZB_HA_MAINS_POWER_OUTLET_DEVICE_ID,
		.app_device_version = 0,
	};
	ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(endpoints, clusters, endpoint_config));
	ESP_ERROR_CHECK(esp_zb_device_register(endpoints));
	esp_zb_core_action_handler_register(zigbee_action_handler);
	ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(ZIGBEE_PRIMARY_CHANNEL_MASK));
	ESP_ERROR_CHECK(esp_zb_start(false));
	esp_zb_stack_main_loop();
}

void app_main(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	} else {
		ESP_ERROR_CHECK(err);
	}

	oled_init();
	display_state_queue = xQueueCreate(1, sizeof(bool));
	ESP_ERROR_CHECK(display_state_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
	oled_render_state(false);
	BaseType_t task_created = xTaskCreate(oled_state_task, "oled_state", 4096, NULL, 4, NULL);
	ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

	esp_zb_platform_config_t platform_config = {
		.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE,
		.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
	};
	ESP_ERROR_CHECK(esp_zb_platform_config(&platform_config));

	task_created = xTaskCreate(zigbee_task, "Zigbee_main", 4096, NULL, 5, NULL);
	ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
