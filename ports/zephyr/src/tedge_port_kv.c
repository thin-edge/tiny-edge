/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of <tedge/port/kv.h>: the settings subsystem, with
 * whatever backend the application configured (NVS on the boards this
 * client ships on).
 */

#include <tedge/port/kv.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <errno.h>

int tedge_port_kv_init(void)
{
	return settings_subsys_init();
}

int tedge_port_kv_set(const char *key, const void *value, size_t len)
{
	return settings_save_one(key, value, len);
}

struct get_ctx {
	void *buf;
	size_t cap;
	int rc;
};

static int get_cb(const char *key, size_t len, settings_read_cb read_cb,
		  void *cb_arg, void *param)
{
	struct get_ctx *ctx = param;
	ssize_t n;

	if (key != NULL) {
		return 0; /* below the key asked for, not the key itself */
	}
	if (len > ctx->cap) {
		ctx->rc = -ENOSPC;
		return 0;
	}
	n = read_cb(cb_arg, ctx->buf, len);
	ctx->rc = (int)n;
	return 0;
}

int tedge_port_kv_get(const char *key, void *buf, size_t cap)
{
	struct get_ctx ctx = { .buf = buf, .cap = cap, .rc = -ENOENT };
	int rc = settings_load_subtree_direct(key, get_cb, &ctx);

	return (rc != 0) ? rc : ctx.rc;
}

int tedge_port_kv_delete(const char *key)
{
	return settings_delete(key);
}

struct foreach_ctx {
	void (*cb)(const char *subkey, void *ctx);
	void *ctx;
};

static int foreach_cb(const char *key, size_t len, settings_read_cb read_cb,
		      void *cb_arg, void *param)
{
	struct foreach_ctx *ctx = param;

	ARG_UNUSED(len);
	ARG_UNUSED(read_cb);
	ARG_UNUSED(cb_arg);
	if (key != NULL) {
		ctx->cb(key, ctx->ctx);
	}
	return 0;
}

int tedge_port_kv_foreach(const char *prefix,
			  void (*cb)(const char *subkey, void *ctx), void *ctx)
{
	struct foreach_ctx fctx = { .cb = cb, .ctx = ctx };

	return settings_load_subtree_direct(prefix, foreach_cb, &fctx);
}
