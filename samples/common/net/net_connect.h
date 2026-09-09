/** @file net_connect.h
 *  @brief Board network bring-up shared by every sample.
 *
 *  ../net.cmake compiles net_events.c plus one implementation per board
 *  family: lte.c (nRF91 modem), wifi.c (ESP32-C6 station) or native_sim.c
 *  (host sockets).
 */
#ifndef NET_CONNECT_H
#define NET_CONNECT_H

#include <stddef.h>
#include <zephyr/kernel.h>

/** Brings this board's credential store to a writable state; idempotent.
 *  Call it before pigeon_init() and net_connect(): a modem reaches its store
 *  over AT commands, which need the modem library running and the modem
 *  offline. */
int net_prepare(void);

/** Brings the interface up and blocks until it has connectivity. */
int net_connect(void);

/** Given once the interface reports L4 connectivity. */
extern struct k_sem network_connection_sem;

/** Registers the connectivity event handlers and clears any connect left
 *  pending by an earlier attempt. Call it at the top of net_connect(). */
void net_events_arm(void);

/** Takes the interface down; on a modem this includes the graceful power-off. */
int net_disconnect(void);

/** Installs a PEM CA certificate under sec_tag for this board's TLS stack.
 *  Call it before net_connect(): a modem's store only accepts writes offline. */
int net_install_ca(int sec_tag, const char* pem, size_t len);

#endif
