/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The network: the port interface.
 *
 * The application owns connectivity: it brings the interface up and keeps
 * it up. The client only watches whether it is up, and opens its own
 * connections over it. The port provides those connections as byte
 * streams (TCP, or TLS on top of TCP) and datagrams (UDP, for mDNS), with
 * the RTOS's sockets and its usual TLS stack. The core speaks MQTT, HTTP
 * and WebSocket over them itself.
 *
 * IPv4 only, as the client is today. An address is 4 bytes in network
 * order. A connection is a non-negative int handle of the port's choosing
 * (a socket descriptor, as a rule); -1 is no connection.
 *
 * Calls come from the client's threads. Each connection is used by one
 * thread at a time.
 */

#ifndef TEDGE_PORT_NET_H_
#define TEDGE_PORT_NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Connectivity -------------------------------------------------------- */

/** True when the interface the client uses has an IPv4 address. */
bool tedge_port_net_is_up(void);

/**
 * Call @p cb whenever the network comes up (true) or goes down (false).
 * Called once, from tedge_init(). @p cb may run in an RTOS callback
 * context: it only sets flags.
 */
void tedge_port_net_watch(void (*cb)(bool up));

/* --- Names --------------------------------------------------------------- */

/**
 * Look up the IPv4 address of @p host with the platform's resolver (DNS,
 * as the application configured it). A dotted address is returned as is.
 *
 * @return 0 with @p addr set; -ETIMEDOUT when nobody answered in time;
 *         -EAGAIN when the resolver could not ask (no server yet, no
 *         memory), worth another try; -ENOENT when the name has no
 *         address, which the next try will say again.
 */
int tedge_port_resolve(const char *host, uint8_t addr[4],
		       uint32_t timeout_ms);

/** Parse a dotted IPv4 address. @return true when @p text is one. */
bool tedge_port_ipv4_parse(const char *text, uint8_t addr[4]);

/** True when @p addr is one of this device's own addresses. */
bool tedge_port_net_is_own(const uint8_t addr[4]);

/** True when @p addr is on the subnet of the interface the client uses. */
bool tedge_port_net_on_subnet(const uint8_t addr[4]);

/* --- TLS credentials ----------------------------------------------------- */

/* The port holds the credentials its TLS stack verifies and presents. A
 * buffer passed here must stay valid until it is replaced or cleared:
 * some stacks keep the pointer (Zephyr's TLS credentials do). */

/** The trust anchors for the server: one or more PEM certificates, NUL
 *  included. @return 0, or a negative errno. */
int tedge_port_tls_set_ca(const void *pem, size_t len);

/** The device's own certificate, DER, replacing any earlier one. The
 *  port presents it with the device key (crypto.h), which it keeps.
 *  @return 0, or a negative errno. */
int tedge_port_tls_set_device(const void *cert, size_t cert_len);

/** Forget the device's certificate and key. */
void tedge_port_tls_clear_device(void);

/* --- Connections --------------------------------------------------------- */

/** How a stream connection is secured. */
enum tedge_port_security {
	/** Plain TCP. */
	TEDGE_PORT_TCP,
	/** TLS 1.2, verifying the server against the CA set above. */
	TEDGE_PORT_TLS,
	/** TLS 1.2 as above, presenting the device's certificate too. */
	TEDGE_PORT_TLS_DEVICE,
};

/**
 * Open a stream to @p addr : @p port, within @p timeout_ms including the
 * TLS handshake.
 *
 * @param hostname For TLS: the name the server's certificate must carry,
 *        and the SNI. Ignored for plain TCP.
 * @return A connection handle, or a negative errno.
 */
int tedge_port_connect(const uint8_t addr[4], uint16_t port,
		       enum tedge_port_security security, const char *hostname,
		       uint32_t timeout_ms);

/** Open a UDP socket on an ephemeral port. @return A handle, or a
 *  negative errno. */
int tedge_port_udp_open(void);

/** Send one datagram to @p addr : @p port. @return 0, or a negative errno. */
int tedge_port_udp_sendto(int conn, const void *buf, size_t len,
			  const uint8_t addr[4], uint16_t port);

/** Disable Nagle's algorithm on a TCP or TLS connection, for traffic that
 *  is interactive. @return 0, or a negative errno. */
int tedge_port_set_nodelay(int conn);

/**
 * Send up to @p len bytes. Waits until at least one byte is sent.
 *
 * @return The bytes sent, or a negative errno.
 */
int tedge_port_send(int conn, const void *buf, size_t len);

/**
 * Receive up to @p len bytes, waiting up to @p timeout_ms for the first.
 * With 0, returns at once.
 *
 * @return The bytes received; 0 when the peer closed the connection;
 *         -EAGAIN when nothing arrived in time; or another negative errno.
 */
int tedge_port_recv(int conn, void *buf, size_t len, uint32_t timeout_ms);

/** What tedge_port_poll() waits for, and reports. */
#define TEDGE_PORT_POLLIN  0x1 /**< data, or the peer's close, to read */
#define TEDGE_PORT_POLLERR 0x2 /**< reported only: the connection failed */
#define TEDGE_PORT_POLLHUP 0x4 /**< reported only: the peer hung up */

struct tedge_port_pollfd {
	int conn;
	uint8_t events;  /**< TEDGE_PORT_POLLIN */
	uint8_t revents; /**< set by tedge_port_poll() */
};

/**
 * Wait up to @p timeout_ms for any of @p n connections to be readable.
 *
 * @return How many have revents set; 0 on a timeout; or a negative errno.
 */
int tedge_port_poll(struct tedge_port_pollfd *fds, size_t n,
		    uint32_t timeout_ms);

/** Close @p conn. A negative handle is ignored. */
void tedge_port_close(int conn);

#endif /* TEDGE_PORT_NET_H_ */
