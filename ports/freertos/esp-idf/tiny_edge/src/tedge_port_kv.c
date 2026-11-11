/* SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-IDF side of <tedge/port/kv.h>: NVS, in the namespace "tedge" of
 * the default "nvs" partition.
 *
 * An NVS key has at most 15 characters, and the client's keys are paths
 * ("tedge/param/modbus/unit_id"). So each key is stored under a name made
 * from its hash, "t" and 8 hex digits, and the value is a blob holding the
 * full key, a NUL, then the value: a read checks the key it finds, and the
 * listing (tedge_port_kv_foreach) reads the keys back. When two keys share
 * a hash, the second takes the next of PROBES names.
 */

#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include <nvs.h>
#include <nvs_flash.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEDGE_LOG_DECLARE();

#define NAMESPACE "tedge"
#define PROBES 4

static nvs_handle_t nvs;
/* Finding a free name and taking it is one step for writers. */
TEDGE_PORT_MUTEX_DEFINE(write_lock);

static int errno_of(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_NVS_NOT_FOUND:
		return -ENOENT;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
		return -ENOSPC;
	case ESP_ERR_NVS_INVALID_LENGTH:
	case ESP_ERR_NVS_VALUE_TOO_LONG:
		return -EFBIG;
	default:
		return -EIO;
	}
}

/* FNV-1a over the key, and over the probe number. */
static void nvs_name(const char *key, int probe, char name[NVS_KEY_NAME_MAX_SIZE])
{
	uint32_t h = 2166136261u;

	for (const char *p = key; *p != '\0'; p++) {
		h = (h ^ (uint8_t)*p) * 16777619u;
	}
	h = (h ^ (uint32_t)probe) * 16777619u;
	snprintf(name, NVS_KEY_NAME_MAX_SIZE, "t%08lx", (unsigned long)h);
}

/* Read the blob stored as @p name into a buffer of the right size, which
 * the caller frees. @return the buffer, or NULL with *err set. */
static char *blob_read(const char *name, size_t *len, int *err)
{
	char *buf;
	esp_err_t e = nvs_get_blob(nvs, name, NULL, len);

	if (e != ESP_OK) {
		*err = errno_of(e);
		return NULL;
	}
	buf = malloc(*len + 1);
	if (buf == NULL) {
		*err = -ENOMEM;
		return NULL;
	}
	e = nvs_get_blob(nvs, name, buf, len);
	if (e != ESP_OK) {
		free(buf);
		*err = errno_of(e);
		return NULL;
	}
	buf[*len] = '\0'; /* so a blob without its NUL still ends */
	return buf;
}

/* The name @p key is stored under, or with @p for_write the first free
 * name when it is not stored. @return 0; -ENOENT; -ENOSPC when every name
 * it could take holds another key. */
static int find(const char *key, bool for_write, char name[NVS_KEY_NAME_MAX_SIZE])
{
	char candidate[NVS_KEY_NAME_MAX_SIZE];
	bool have_free = false;

	for (int probe = 0; probe < PROBES; probe++) {
		size_t len;
		int err = 0;
		char *blob;

		nvs_name(key, probe, candidate);
		blob = blob_read(candidate, &len, &err);
		if (blob == NULL) {
			if (err == -ENOENT && for_write && !have_free) {
				memcpy(name, candidate, NVS_KEY_NAME_MAX_SIZE);
				have_free = true;
			}
			if (err != -ENOENT) {
				return err;
			}
			continue;
		}
		bool same = strcmp(blob, key) == 0;

		free(blob);
		if (same) {
			memcpy(name, candidate, NVS_KEY_NAME_MAX_SIZE);
			return 0;
		}
	}
	if (!for_write) {
		return -ENOENT;
	}
	return have_free ? 0 : -ENOSPC;
}

int tedge_port_kv_init(void)
{
	/* The application has usually started NVS already (Wi-Fi needs it);
	 * a second init is harmless. One that needs an erase is not the
	 * client's to do: it would take the application's data with it. */
	esp_err_t e = nvs_flash_init();

	if (e != ESP_OK) {
		TEDGE_LOG_ERR("NVS did not start (%s): the application must "
			      "initialise or erase it", esp_err_to_name(e));
		return -EIO;
	}
	e = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
	if (e != ESP_OK) {
		TEDGE_LOG_ERR("NVS namespace %s did not open (%s)", NAMESPACE,
			      esp_err_to_name(e));
		return errno_of(e);
	}
	return 0;
}

int tedge_port_kv_set(const char *key, const void *value, size_t len)
{
	char name[NVS_KEY_NAME_MAX_SIZE];
	size_t klen = strlen(key) + 1;
	char *blob = malloc(klen + len);
	esp_err_t e;
	int ret;

	if (blob == NULL) {
		return -ENOMEM;
	}
	memcpy(blob, key, klen);
	memcpy(blob + klen, value, len);
	tedge_port_mutex_lock(&write_lock);
	ret = find(key, true, name);
	if (ret == 0) {
		e = nvs_set_blob(nvs, name, blob, klen + len);
		if (e == ESP_OK) {
			e = nvs_commit(nvs);
		}
		ret = errno_of(e);
	}
	tedge_port_mutex_unlock(&write_lock);
	free(blob);
	return ret;
}

int tedge_port_kv_get(const char *key, void *buf, size_t cap)
{
	char name[NVS_KEY_NAME_MAX_SIZE];
	size_t len, klen = strlen(key) + 1;
	int ret = find(key, false, name);
	char *blob;

	if (ret != 0) {
		return ret;
	}
	blob = blob_read(name, &len, &ret);
	if (blob == NULL) {
		return ret;
	}
	len -= klen;
	if (len > cap) {
		ret = -ENOSPC;
	} else {
		memcpy(buf, blob + klen, len);
		ret = (int)len;
	}
	free(blob);
	return ret;
}

int tedge_port_kv_delete(const char *key)
{
	char name[NVS_KEY_NAME_MAX_SIZE];
	int ret;

	tedge_port_mutex_lock(&write_lock);
	ret = find(key, false, name);
	if (ret == 0) {
		esp_err_t e = nvs_erase_key(nvs, name);

		if (e == ESP_OK) {
			e = nvs_commit(nvs);
		}
		ret = errno_of(e);
	} else if (ret == -ENOENT) {
		ret = 0;
	}
	tedge_port_mutex_unlock(&write_lock);
	return ret;
}

int tedge_port_kv_foreach(const char *prefix,
			  void (*cb)(const char *subkey, void *ctx), void *ctx)
{
	size_t plen = strlen(prefix);
	nvs_iterator_t it = NULL;
	esp_err_t e;

	e = nvs_entry_find_in_handle(nvs, NVS_TYPE_BLOB, &it);
	while (e == ESP_OK) {
		nvs_entry_info_t info;
		size_t len;
		int err;
		char *blob;

		(void)nvs_entry_info(it, &info);
		blob = blob_read(info.key, &len, &err);
		if (blob != NULL) {
			if (strncmp(blob, prefix, plen) == 0 &&
			    blob[plen] == '/' && blob[plen + 1] != '\0') {
				cb(blob + plen + 1, ctx);
			}
			free(blob);
		}
		e = nvs_entry_next(&it);
	}
	nvs_release_iterator(it);
	return (e == ESP_ERR_NVS_NOT_FOUND) ? 0 : errno_of(e);
}
