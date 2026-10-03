#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

#define OLED_SCL_GPIO GPIO_NUM_5
#define OLED_SDA_GPIO GPIO_NUM_4
#define OLED_WIDTH 128
#define OLED_PAGES 8
#define OLED_ADDRESS_0 0x3C
#define OLED_ADDRESS_1 0x3D

static const char *TAG = "ssd1306";

void app_main(void)
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
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "No SSD1306 found at 0x3C or 0x3D: %s", esp_err_to_name(err));
		return;
	}

	i2c_device_config_t device_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = 100000,
	};
	i2c_master_dev_handle_t display;
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &display));

	const uint8_t init_commands[] = {
		0x00, 0xAE,       // Display off
		0xD5, 0x80,       // Clock divide ratio
		0xA8, 0x3F,       // 64-row multiplex
		0xD3, 0x00,       // Display offset
		0x40,             // Start line
		0x8D, 0x14,       // Enable charge pump
		0x20, 0x02,       // Page addressing mode
		0xA1,             // Segment remap
		0xC8,             // Reverse COM scan
		0xDA, 0x12,       // COM pin configuration
		0x81, 0x7F,       // Contrast
		0xD9, 0xF1,       // Pre-charge period
		0xDB, 0x40,       // VCOMH deselect level
		0xA4,             // Resume display from RAM
		0xA6,             // Normal display
		0xAF,             // Display on
	};
	ESP_ERROR_CHECK(i2c_master_transmit(display, init_commands, sizeof(init_commands), -1));

	uint8_t packet[OLED_WIDTH + 1];
	packet[0] = 0x40;
	for (uint8_t page = 0; page < OLED_PAGES; page++) {
		const uint8_t set_page[] = {0x00, (uint8_t)(0xB0 | page), 0x00, 0x10};
		ESP_ERROR_CHECK(i2c_master_transmit(display, set_page, sizeof(set_page), -1));

		for (uint8_t column = 0; column < OLED_WIDTH; column++) {
			uint8_t pixels = (column == 0 || column == OLED_WIDTH - 1 || column == OLED_WIDTH / 2)
								 ? 0xFF
								 : 0x00;
			if (page == 0) {
				pixels |= 0x01;
			}
			if (page == OLED_PAGES - 1) {
				pixels |= 0x80;
			}
			if (page == OLED_PAGES / 2) {
				pixels |= 0x01;
			}
			packet[column + 1] = pixels;
		}
		ESP_ERROR_CHECK(i2c_master_transmit(display, packet, sizeof(packet), -1));
	}

	ESP_LOGI(TAG, "SSD1306 initialized at 0x%02X on SDA GPIO%d / SCL GPIO%d", address,
			 OLED_SDA_GPIO, OLED_SCL_GPIO);
}
