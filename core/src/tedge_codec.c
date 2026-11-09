/* SPDX-License-Identifier: Apache-2.0
 *
 * Encodings the client needs in plain C, whatever crypto the port has:
 * SHA-1 for the WebSocket handshake's Sec-WebSocket-Accept (not for
 * anything secret), base64 for it and for HTTP basic credentials, and UTC
 * dates as seconds since the epoch.
 */

#include "tedge_internal.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------ */
/* SHA-1                                                                     */
/* ------------------------------------------------------------------------ */

static uint32_t rol(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
	uint32_t w[16];
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	}
	for (int i = 0; i < 80; i++) {
		uint32_t f, k, t;

		if (i >= 16) {
			w[i & 15] = rol(w[(i + 13) & 15] ^ w[(i + 8) & 15] ^
					w[(i + 2) & 15] ^ w[i & 15], 1);
		}
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		t = rol(a, 5) + f + e + k + w[i & 15];
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = t;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
}

void tedge_sha1(const void *data, size_t len, uint8_t digest[20])
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
			  0xC3D2E1F0 };
	const uint8_t *p = data;
	uint8_t block[64];
	uint64_t bits = (uint64_t)len * 8;
	size_t n = len;

	while (n >= 64) {
		sha1_block(h, p);
		p += 64;
		n -= 64;
	}
	memset(block, 0, sizeof(block));
	memcpy(block, p, n);
	block[n] = 0x80;
	if (n >= 56) {
		sha1_block(h, block);
		memset(block, 0, sizeof(block));
	}
	for (int i = 0; i < 8; i++) {
		block[63 - i] = (uint8_t)(bits >> (8 * i));
	}
	sha1_block(h, block);
	for (int i = 0; i < 20; i++) {
		digest[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
	}
}

/* ------------------------------------------------------------------------ */
/* Base64                                                                    */
/* ------------------------------------------------------------------------ */

int tedge_base64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
	static const char abc[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;

	if (cap < ((len + 2) / 3) * 4 + 1) {
		return -ENOSPC;
	}
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < len) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= in[i + 2];
		}
		out[o++] = abc[(v >> 18) & 63];
		out[o++] = abc[(v >> 12) & 63];
		out[o++] = (i + 1 < len) ? abc[(v >> 6) & 63] : '=';
		out[o++] = (i + 2 < len) ? abc[v & 63] : '=';
	}
	out[o] = '\0';
	return (int)o;
}

/* ------------------------------------------------------------------------ */
/* Dates                                                                     */
/* ------------------------------------------------------------------------ */

/* Seconds since the epoch for a UTC date and time, which C99 has no
 * function for (timegm() is not standard). Days from the civil date as in
 * Howard Hinnant's days_from_civil(). */
time_t tedge_timegm(const struct tm *tm)
{
	int64_t y = (int64_t)tm->tm_year + 1900 - (tm->tm_mon < 2);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t m = tm->tm_mon + 1;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	int64_t days = era * 146097 + doe - 719468;

	return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 +
			tm->tm_sec);
}
