/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Setting a device up over a cable or Bluetooth (TEDGE_PROVISIONING).
 *
 * An optional library beside the client: it answers the Improv Wi-Fi
 * protocol (https://www.improv-wifi.com), which tools such as
 * `tedge-onboard`, ESP Web Tools and Improv phone apps speak, and hands what
 * it receives to the application (the Wi-Fi network) and to the client (the
 * Cumulocity tenant and one-time password, through tedge_set_c8y_url() and
 * tedge_set_enroll_otp()). The client itself knows nothing about it.
 *
 * The application calls tedge_provisioning_init() after tedge_init(). The
 * port connects a transport: for Improv Serial, it feeds the console's
 * bytes to tedge_provisioning_serial_feed() and writes what the library
 * gives its write function.
 *
 * Besides the Improv commands, the library answers three of tiny-edge's
 * own (TEDGE_PROVISIONING_CMD_*): what onboarding needs to know about the
 * device, the onboarding data itself, and a request to identify the board.
 */

#ifndef TEDGE_PROVISIONING_H_
#define TEDGE_PROVISIONING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The version of tiny-edge's commands, reported by the info command. A
 *  tool that knows an older version refuses a newer device. */
#define TEDGE_PROVISIONING_PROTOCOL_VERSION 1

/** tiny-edge's RPC commands, numbered away from Improv's own. */
#define TEDGE_PROVISIONING_CMD_INFO     0xE0
#define TEDGE_PROVISIONING_CMD_ONBOARD  0xE1
#define TEDGE_PROVISIONING_CMD_IDENTIFY 0xE2

/** In the onboard command's flags: set the device up again even though it
 *  is connected (it gets a new certificate). */
#define TEDGE_PROVISIONING_FLAG_REONBOARD 0x01

/** A Wi-Fi network the board can see (tedge_provisioning_hooks::wifi_scan). */
struct tedge_provisioning_network {
	char ssid[33];
	int8_t rssi;   /**< dBm */
	bool secured;  /**< needs a password */
};

/** What the application provides. Every member may be NULL. */
struct tedge_provisioning_hooks {
	/**
	 * Join @p ssid with @p password (empty for an open network) and keep
	 * them for the next boot, in the application's own way. Block until
	 * the network is joined or the attempt failed, for 30 s at most.
	 * Without it, every Wi-Fi request is answered "unable to connect".
	 *
	 * @return 0 once joined, or a negative errno.
	 */
	int (*wifi_connect)(const char *ssid, const char *password, void *ctx);

	/** Make this board recognisable for about 10 s (blink an LED, say).
	 *  The library also logs a line naming the device. */
	void (*identify)(void *ctx);

	/**
	 * Scan for Wi-Fi networks, filling up to @p max entries of @p out.
	 * Block until the scan is done, for 15 s at most. A tool uses it to
	 * offer the networks the board can actually reach. Without it, the
	 * scan request is answered "unknown RPC".
	 *
	 * @return The number of networks found, or a negative errno.
	 */
	int (*wifi_scan)(struct tedge_provisioning_network *out, size_t max,
			 void *ctx);

	/** The chip or board, for Improv's device info ("ESP32-S3"). NULL
	 *  reports "unknown". */
	const char *chip;

	void *ctx;
};

/**
 * Start the library: its worker thread, and the state it reports. Call once,
 * after tedge_init(). @p hooks is kept, so it must stay valid.
 *
 * @return 0, or a negative errno.
 */
int tedge_provisioning_init(const struct tedge_provisioning_hooks *hooks);

/* --- Improv Serial (TEDGE_PROVISIONING_SERIAL) --------------------------- */

/**
 * Writes @p len bytes to the console, all of them, as one unit: no other
 * output (log lines) may land in between. Called from the library's
 * worker thread.
 */
typedef void (*tedge_provisioning_write_t)(const void *data, size_t len,
					   void *ctx);

/** Connect the serial transport: the library writes its frames with
 *  @p write. NULL disconnects it. */
void tedge_provisioning_serial_attach(tedge_provisioning_write_t write,
				      void *ctx);

/**
 * Feed bytes received on the console. Bytes outside a valid Improv frame
 * (typed text, a tool's other output) are skipped. May be called from any
 * thread; complete frames are handled on the library's worker thread.
 *
 * @return How many valid frames these bytes completed.
 */
size_t tedge_provisioning_serial_feed(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TEDGE_PROVISIONING_H_ */
