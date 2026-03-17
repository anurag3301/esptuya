#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "nvs_flash.h"
#include "ssd1306.h"
#include "tuya_client.hpp"
#include "wifi_cred.hpp"

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const char *TAG = "app_main";
static const char *BTN_TAG = "buttons";

static constexpr gpio_num_t kButtonPins[] = {GPIO_NUM_14, GPIO_NUM_27, GPIO_NUM_25};
static constexpr gpio_num_t kI2cSdaPin = GPIO_NUM_21;
static constexpr gpio_num_t kI2cSclPin = GPIO_NUM_22;
static constexpr i2c_port_t kI2cPort = I2C_NUM_0;
static QueueHandle_t s_button_evt_queue = nullptr;
static QueueHandle_t s_dp_cmd_queue = nullptr;
static OLED_Config s_oled_cfg{};

// Tuya devices (extendable list)
static TuyaDeviceConfig kTuyaDevices[] = {
    {"d70e605179edee94404oie", "bcZx:vssLzn6qFds", "192.168.0.100", "3.5"},
};

extern "C" bool tuya_send_dp_bool(int dp, bool value)
{
	if (!s_dp_cmd_queue)
		return false;
	DpCommand cmd{};
	cmd.type = DpCommand::Type::BOOL;
	cmd.dp = dp;
	cmd.bool_val = value;
	return xQueueSend(s_dp_cmd_queue, &cmd, 0) == pdTRUE;
}

extern "C" bool tuya_send_dp_int(int dp, int value)
{
	if (!s_dp_cmd_queue)
		return false;
	DpCommand cmd{};
	cmd.type = DpCommand::Type::INT;
	cmd.dp = dp;
	cmd.int_val = value;
	return xQueueSend(s_dp_cmd_queue, &cmd, 0) == pdTRUE;
}

extern "C" bool tuya_send_dp_string(int dp, const char *value)
{
	if (!s_dp_cmd_queue)
		return false;
	DpCommand cmd{};
	cmd.type = DpCommand::Type::STRING;
	cmd.dp = dp;
	strncpy(cmd.str_val, value ? value : "", sizeof(cmd.str_val) - 1);
	return xQueueSend(s_dp_cmd_queue, &cmd, 0) == pdTRUE;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
		ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event ? event->reason : -1);
		esp_wifi_connect();
		xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
	uint32_t gpio_num = (uint32_t)arg;
	if (s_button_evt_queue) {
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		xQueueSendFromISR(s_button_evt_queue, &gpio_num, &xHigherPriorityTaskWoken);
		if (xHigherPriorityTaskWoken) {
			portYIELD_FROM_ISR();
		}
	}
}

static void button_task(void *arg)
{
	uint32_t gpio_num;
	int64_t last_seen_us[sizeof(kButtonPins) / sizeof(kButtonPins[0])] = {0};
	while (true) {
		if (xQueueReceive(s_button_evt_queue, &gpio_num, portMAX_DELAY)) {
			int idx = -1;
			for (size_t i = 0; i < sizeof(kButtonPins) / sizeof(kButtonPins[0]); ++i) {
				if (kButtonPins[i] == gpio_num) {
					idx = (int)i;
					break;
				}
			}
			int64_t now = esp_timer_get_time();
			if (idx >= 0) {
				if (now - last_seen_us[idx] < 200000) {  // debounce 200ms
					continue;
				}
				last_seen_us[idx] = now;
			}

			ESP_LOGI(BTN_TAG, "Button press detected on GPIO %ld", (long)gpio_num);

			if (gpio_num == GPIO_NUM_27 || gpio_num == GPIO_NUM_25) {
				tuya_send_dp_bool(20, gpio_num == GPIO_NUM_27);
			}
		}
	}
}

static void initialise_buttons()
{
	gpio_config_t io_conf = {};
	io_conf.intr_type = GPIO_INTR_NEGEDGE;  // falling edge
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	io_conf.pin_bit_mask = 0;
	for (auto pin : kButtonPins) {
		io_conf.pin_bit_mask |= (1ULL << pin);
	}
	gpio_config(&io_conf);

	s_button_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	s_dp_cmd_queue = xQueueCreate(5, sizeof(DpCommand));
	gpio_install_isr_service(0);
	for (auto pin : kButtonPins) {
		gpio_isr_handler_add(pin, button_isr_handler, (void *)pin);
	}

	xTaskCreatePinnedToCore(button_task, "button_task", 2048, nullptr, 5, nullptr, 1);
}

static void initialise_wifi()
{
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(
	    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
	ESP_ERROR_CHECK(
	    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

	wifi_config_t wifi_config = {};
	strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
	strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD,
	        sizeof(wifi_config.sta.password) - 1);
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	wifi_config.sta.pmf_cfg.capable = true;
	wifi_config.sta.pmf_cfg.required = false;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	// Keep radio awake to avoid losing connection on busy APs
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "Connecting to %s ...", WIFI_SSID);
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
	                                       portMAX_DELAY);
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "WiFi connected");
	}
}

static void initialise_i2c()
{
	i2c_config_t conf = {};
	conf.mode = I2C_MODE_MASTER;
	conf.sda_io_num = kI2cSdaPin;
	conf.scl_io_num = kI2cSclPin;
	conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
	conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
	conf.master.clk_speed = 100000;  // 100kHz

	ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
	ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0));
}

static int32_t oled_send_i2c(void *user_context, uint8_t i2c_address_7bit, const uint8_t *data,
                             size_t length)
{
	if (!data || length == 0) {
		return OLED_ERR_INVALID_ARG;
	}

	i2c_port_t port = static_cast<i2c_port_t>(reinterpret_cast<intptr_t>(user_context));
	esp_err_t err = i2c_master_write_to_device(port, i2c_address_7bit, data, length,
	                                           pdMS_TO_TICKS(50));
	return (err == ESP_OK) ? OLED_OK : OLED_ERR_IO;
}

static void initialise_oled()
{
	s_oled_cfg.bus_type = OLED_BUS_I2C;
	s_oled_cfg.width = 128;
	s_oled_cfg.height = 32;
	s_oled_cfg.user_context = reinterpret_cast<void *>(static_cast<intptr_t>(kI2cPort));
	s_oled_cfg.transport.i2c.i2c_address_7bit = 0x3C;
	s_oled_cfg.transport.i2c.send_fn = oled_send_i2c;

	int32_t init_status = OLED_Init(&s_oled_cfg);
	if (init_status != OLED_OK) {
		ESP_LOGE(TAG, "OLED init failed: %ld", (long)init_status);
		return;
	}

	if (OLED_Fill(&s_oled_cfg, 0xFF) != OLED_OK) {
		ESP_LOGE(TAG, "OLED fill failed");
	} else {
		ESP_LOGI(TAG, "OLED initialized and filled");
	}
}

static void scan_i2c_bus()
{
	ESP_LOGI(TAG, "Scanning I2C bus (SDA=%d, SCL=%d)", (int)kI2cSdaPin, (int)kI2cSclPin);
	for (uint8_t addr = 1; addr < 0x7F; ++addr) {
		i2c_cmd_handle_t cmd = i2c_cmd_link_create();
		i2c_master_start(cmd);
		i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
		i2c_master_stop(cmd);
		esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
		i2c_cmd_link_delete(cmd);
		if (err == ESP_OK) {
			ESP_LOGI(TAG, "Found I2C device at address 0x%02X", addr);
		}
	}
	ESP_LOGI(TAG, "I2C scan complete");
}

extern "C" void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}

	initialise_i2c();
	scan_i2c_bus();
	initialise_oled();
	initialise_wifi();
	initialise_buttons();

	auto *cfg = new TuyaDeviceConfig{kTuyaDevices[0]};

	ESP_LOGI(TAG, "Starting Tuya monitor for %s at %s", cfg->id.c_str(), cfg->address.c_str());

	auto tuya_task = [](void *arg) {
		auto *bundle = static_cast<std::pair<TuyaDeviceConfig *, QueueHandle_t> *>(arg);
		std::unique_ptr<TuyaDeviceConfig> cfg_ptr(bundle->first);
		QueueHandle_t cmd_queue = bundle->second;
		delete bundle;
		TuyaClient client(*cfg_ptr, cmd_queue);
		client.monitor_loop();  // Never returns
		vTaskDelete(nullptr);
	};

	// Larger stack for crypto/select buffers; pin to core 1 to leave Wi-Fi on core 0
	auto *bundle = new std::pair<TuyaDeviceConfig *, QueueHandle_t>(cfg, s_dp_cmd_queue);
	xTaskCreatePinnedToCore(tuya_task, "tuya_monitor", 12288, bundle, 5, nullptr, 1);
}
