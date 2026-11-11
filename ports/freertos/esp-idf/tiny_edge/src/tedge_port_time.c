/* SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-IDF side of <tedge/port/time.h>: the system clock, set with
 * ESP-IDF's SNTP client (esp_netif_sntp) when nobody else has set it.
 */

#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/time.h>

#include <esp_netif_sntp.h>
#include <esp_sntp.h>

#include <errno.h>
#include <sys/time.h>

TEDGE_LOG_DECLARE();

/* Anything before 2025-01-01 means "not set". */
#define CLOCK_FLOOR 1735689600

/* How long one sync waits for an answer. */
#define SYNC_TIMEOUT_MS 10000

bool tedge_port_time_is_valid(void)
{
	return time(NULL) > CLOCK_FLOOR;
}

int tedge_port_time_sync(void)
{
#if defined(TEDGE_SNTP)
	static bool started;
	esp_err_t e;

	if (tedge_port_time_is_valid()) {
		return 0;
	}
	/* Started once and left running, so it keeps the clock right. When
	 * the application runs SNTP itself, the clock is valid by now, or
	 * its client is the one to wait for. */
	if (!started && !esp_sntp_enabled()) {
		esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(TEDGE_SNTP_SERVER);

		e = esp_netif_sntp_init(&cfg);
		if (e != ESP_OK) {
			TEDGE_LOG_WRN("SNTP (%s) did not start (%s)",
				      TEDGE_SNTP_SERVER, esp_err_to_name(e));
			return -EIO;
		}
		started = true;
	}
	if (started) {
		e = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SYNC_TIMEOUT_MS));
		if (e == ESP_OK || tedge_port_time_is_valid()) {
			TEDGE_LOG_INF("clock set from %s", TEDGE_SNTP_SERVER);
			return 0;
		}
		TEDGE_LOG_WRN("SNTP (%s): no answer yet", TEDGE_SNTP_SERVER);
		return -ETIMEDOUT;
	}
	return tedge_port_time_is_valid() ? 0 : -EAGAIN;
#else
	if (tedge_port_time_is_valid()) {
		return 0;
	}
	TEDGE_LOG_WRN("the clock is not set and %s is off: the application "
		      "must set it before the client can connect",
		      TEDGE_CONFIG_NAME(TEDGE_SNTP));
	return -ENOTSUP;
#endif
}

int tedge_port_time_now(time_t *secs)
{
	time_t now = time(NULL);

	if (now <= CLOCK_FLOOR) {
		return -EAGAIN;
	}
	*secs = now;
	return 0;
}
