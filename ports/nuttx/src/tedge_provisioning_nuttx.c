/* SPDX-License-Identifier: Apache-2.0
 *
 * Set-up over USB on NuttX (<tedge/nuttx.h>): Improv Serial on the
 * console, which is the board's USB serial port, so tedge-onboard finds
 * the board as it finds one on the other ports.
 *
 * Improv frames are binary, and NuttX's console is a terminal: it echoes,
 * collects lines, turns CR into LF on input and LF into CR LF on output.
 * Input is switched to raw once, so the console can no longer run a shell.
 * Output keeps its LF to CR LF for log lines; each frame is written with
 * that turned off, while stdout and stderr are locked so no log line lands
 * inside it. The switch is TCSANOW, not TCSADRAIN: NuttX applies LF to CR
 * LF as bytes enter the driver's buffer, so nothing already queued changes,
 * and the ESP chips' USB Serial/JTAG driver has no txempty operation, so
 * draining calls a null pointer.
 */

#include <nuttx/config.h>

#include <tedge/nuttx.h>
#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>
#include <tedge/provisioning.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

TEDGE_LOG_DECLARE();

#if defined(TEDGE_PROVISIONING_SERIAL)

#define CONSOLE "/dev/console"

TEDGE_PORT_STACK_DEFINE(rx_stack, 3072);
static tedge_port_thread_t rx_thread;
static int console_fd = -1;

static void console_write(const void *data, size_t len, void *ctx)
{
	const uint8_t *p = data;
	struct termios tio;
	tcflag_t oflag;

	(void)ctx;
	(void)fflush(stdout);
	(void)fflush(stderr);
	flockfile(stdout);
	flockfile(stderr);
	if (tcgetattr(console_fd, &tio) == 0) {
		oflag = tio.c_oflag;
		tio.c_oflag &= ~(tcflag_t)(OPOST | ONLCR);
		(void)tcsetattr(console_fd, TCSANOW, &tio);
		tio.c_oflag = oflag;
	}
	while (len > 0) {
		ssize_t n = write(console_fd, p, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		p += n;
		len -= (size_t)n;
	}
	(void)tcsetattr(console_fd, TCSANOW, &tio);
	funlockfile(stderr);
	funlockfile(stdout);
}

static void rx_loop(void *arg)
{
	uint8_t buf[64];

	(void)arg;
	for (;;) {
		ssize_t n = read(console_fd, buf, sizeof(buf));

		if (n > 0) {
			(void)tedge_provisioning_serial_feed(buf, (size_t)n);
		} else if (n < 0 && errno != EINTR) {
			usleep(100 * 1000);
		}
	}
}

static int serial_start(void)
{
	struct termios tio;

	console_fd = open(CONSOLE, O_RDWR);
	if (console_fd < 0) {
		TEDGE_LOG_ERR("provisioning: cannot open %s (%d)", CONSOLE,
			      errno);
		return -errno;
	}
	if (tcgetattr(console_fd, &tio) != 0) {
		TEDGE_LOG_ERR("provisioning: the console has no terminal "
			      "settings; set CONFIG_SERIAL_TERMIOS");
		return -ENOTSUP;
	}
	tio.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
	tio.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR);
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(console_fd, TCSANOW, &tio) != 0) {
		return -errno;
	}
	tedge_provisioning_serial_attach(console_write, NULL);
	return tedge_port_thread_start(&rx_thread, rx_stack,
				       TEDGE_PORT_STACK_SIZEOF(rx_stack),
				       rx_loop, NULL, "tedge_improv");
}

#endif /* TEDGE_PROVISIONING_SERIAL */

#if defined(TEDGE_PROVISIONING)

static int wifi_connect(const char *ssid, const char *password, void *ctx)
{
	(void)ctx;
	return tedge_nuttx_wifi_join(ssid, password);
}

static int wifi_scan(struct tedge_provisioning_network *out, size_t max,
		     void *ctx)
{
	(void)ctx;
	return tedge_nuttx_wifi_scan(out, max);
}

int tedge_provisioning_nuttx_init(const struct tedge_provisioning_hooks *overrides)
{
	static struct tedge_provisioning_hooks hooks;
	int ret;

	hooks = (struct tedge_provisioning_hooks){
		.wifi_connect = wifi_connect,
		.wifi_scan = wifi_scan,
		.chip = CONFIG_ARCH_CHIP,
	};
	if (overrides != NULL) {
		if (overrides->wifi_connect != NULL) {
			hooks.wifi_connect = overrides->wifi_connect;
		}
		if (overrides->identify != NULL) {
			hooks.identify = overrides->identify;
		}
		if (overrides->wifi_scan != NULL) {
			hooks.wifi_scan = overrides->wifi_scan;
		}
		if (overrides->chip != NULL) {
			hooks.chip = overrides->chip;
		}
		if (overrides->firmware_name != NULL) {
			hooks.firmware_name = overrides->firmware_name;
		}
		if (overrides->firmware_version != NULL) {
			hooks.firmware_version = overrides->firmware_version;
		}
		hooks.ctx = overrides->ctx;
	}
	ret = tedge_provisioning_init(&hooks);
	if (ret != 0) {
		return ret;
	}
#if defined(TEDGE_PROVISIONING_SERIAL)
	return serial_start();
#else
	return 0;
#endif
}

#else

int tedge_provisioning_nuttx_init(const struct tedge_provisioning_hooks *overrides)
{
	(void)overrides;
	return -ENOTSUP;
}

#endif /* TEDGE_PROVISIONING */
