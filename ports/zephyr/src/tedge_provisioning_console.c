/* SPDX-License-Identifier: Apache-2.0
 *
 * Improv Serial on the Zephyr console, for the provisioning library
 * (include/tedge/provisioning.h).
 *
 * With the shell on the console (its serial backend), the console is the
 * shell's, and "tedge improv" lends it
 * to a provisioning tool: the shell's bypass mode passes every received
 * byte to the library, until no valid frame has arrived for
 * CONFIG_TEDGE_PROVISIONING_SERIAL_IDLE_S. Frames go out through the
 * shell's own transport, holding the lock its log output takes, so a log
 * line never lands inside a frame, and unchanged (the shell's printing
 * would turn a 0x0A byte into CR LF).
 *
 * Otherwise (no shell, or a shell with only other backends, such as the
 * dummy one the remote shell command uses), the console speaks Improv from
 * boot: its UART is read
 * by polling, which every UART driver supports, and a frame is written
 * with the scheduler locked, so the log thread cannot cut in.
 */

#include <tedge/provisioning.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_SHELL_BACKEND_SERIAL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#if defined(CONFIG_SHELL_BACKEND_SERIAL)

static const struct shell *lent; /* the shell whose console is lent */

static void give_back(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(idle_work, give_back);

static void shell_write(const void *data, size_t len, void *ctx)
{
	const struct shell *sh = ctx;
	const uint8_t *p = data;
	int spins = 0;

	(void)k_sem_take(&sh->ctx->lock_sem, K_FOREVER);
	while (len > 0) {
		size_t n = 0;

		if (sh->iface->api->write(sh->iface, p, len, &n) != 0) {
			break;
		}
		if (n == 0) {
			/* The transport's buffer is full: let it drain, for
			 * a second at most. */
			if (++spins > 1000) {
				break;
			}
			k_msleep(1);
			continue;
		}
		p += n;
		len -= n;
	}
	k_sem_give(&sh->ctx->lock_sem);
}

/* Runs on the shell's thread, with every byte the console received. */
static void bypass(const struct shell *sh, uint8_t *data, size_t len,
		   void *user_data)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(user_data);
	if (tedge_provisioning_serial_feed(data, len) > 0) {
		(void)k_work_reschedule(&idle_work,
					K_SECONDS(CONFIG_TEDGE_PROVISIONING_SERIAL_IDLE_S));
	}
}

static void give_back(struct k_work *work)
{
	const struct shell *sh = lent;

	ARG_UNUSED(work);
	if (sh == NULL) {
		return;
	}
	lent = NULL;
	tedge_provisioning_serial_attach(NULL, NULL);
	shell_set_bypass(sh, NULL, NULL);
	LOG_INF("provisioning: the console is the shell's again");
}

static int cmd_improv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "The console speaks Improv now. It returns to the shell "
		    "after %d s without a request.",
		    CONFIG_TEDGE_PROVISIONING_SERIAL_IDLE_S);
	lent = sh;
	tedge_provisioning_serial_attach(shell_write, (void *)sh);
	shell_set_bypass(sh, bypass, NULL);
	(void)k_work_reschedule(&idle_work,
				K_SECONDS(CONFIG_TEDGE_PROVISIONING_SERIAL_IDLE_S));
	return 0;
}

SHELL_SUBCMD_ADD((tedge), improv, NULL,
		 "Lend the console to a setup tool such as tedge-onboard",
		 cmd_improv, 1, 0);

#else /* !CONFIG_SHELL_BACKEND_SERIAL */

#define POLL_MS 20

static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void uart_write(const void *data, size_t len, void *ctx)
{
	const uint8_t *p = data;

	ARG_UNUSED(ctx);
	k_sched_lock();
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(console, p[i]);
	}
	k_sched_unlock();
}

static void poll_rx(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rx_work, poll_rx);

static void poll_rx(struct k_work *work)
{
	uint8_t buf[64];
	size_t n = 0;
	unsigned char c;

	ARG_UNUSED(work);
	while (n < sizeof(buf) && uart_poll_in(console, &c) == 0) {
		buf[n++] = c;
	}
	if (n > 0) {
		(void)tedge_provisioning_serial_feed(buf, n);
	}
	(void)k_work_schedule(&rx_work, K_MSEC(n == sizeof(buf) ? 0 : POLL_MS));
}

static int console_init(void)
{
	if (!device_is_ready(console)) {
		return -ENODEV;
	}
	tedge_provisioning_serial_attach(uart_write, NULL);
	(void)k_work_schedule(&rx_work, K_MSEC(POLL_MS));
	return 0;
}

SYS_INIT(console_init, APPLICATION, 99);

#endif /* CONFIG_SHELL_BACKEND_SERIAL */
