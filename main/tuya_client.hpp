#pragma once

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct TuyaDeviceConfig {
	std::string id;
	std::string key;
	std::string address;
	std::string version;
};

struct DpCommand {
	enum class Type {
		BOOL,
		INT,
		STRING
	} type;
	int dp;
	bool bool_val;
	int int_val;
	char str_val[64];
};

class TuyaClient {
public:
	explicit TuyaClient(const TuyaDeviceConfig &config, QueueHandle_t cmd_queue);
	void monitor_loop();

private:
	TuyaDeviceConfig cfg_;
	QueueHandle_t cmd_queue_;
};
