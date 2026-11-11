/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF implementation of the logging macros in <tedge/port/log.h>: the
 * client logs with ESP_LOG under the tag "tedge", so `esp_log_level_set()`
 * and the console treat its lines as the application's. A line below
 * CONFIG_TEDGE_LOG_LEVEL is compiled out. */

#ifndef TEDGE_PORT_ESP_IDF_LOG_H_
#define TEDGE_PORT_ESP_IDF_LOG_H_

#include <esp_log.h>

/* The tag, defined once by TEDGE_LOG_REGISTER() (in tedge_core.c). */
extern const char tedge_log_tag[];

#define TEDGE_LOG_REGISTER() const char tedge_log_tag[] = "tedge"
#define TEDGE_LOG_DECLARE() extern const char tedge_log_tag[]

#define TEDGE_LOG_AT_(level, esp_macro, ...)                                   \
	do {                                                                   \
		if (TEDGE_LOG_LEVEL >= (level)) {                              \
			esp_macro(tedge_log_tag, __VA_ARGS__);                 \
		}                                                              \
	} while (0)

#define TEDGE_LOG_ERR(...) TEDGE_LOG_AT_(1, ESP_LOGE, __VA_ARGS__)
#define TEDGE_LOG_WRN(...) TEDGE_LOG_AT_(2, ESP_LOGW, __VA_ARGS__)
#define TEDGE_LOG_INF(...) TEDGE_LOG_AT_(3, ESP_LOGI, __VA_ARGS__)
#define TEDGE_LOG_DBG(...) TEDGE_LOG_AT_(4, ESP_LOGD, __VA_ARGS__)

/* esp_log_level_set() changes a tag's level while the image runs. */
#define TEDGE_PORT_LOG_RUNTIME_LEVEL 1

#endif /* TEDGE_PORT_ESP_IDF_LOG_H_ */
