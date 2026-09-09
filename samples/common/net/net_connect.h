/** @file net_connect.h
 *  @brief Board network bring-up shared by every sample.
 *
 *  ../net.cmake compiles one implementation per board family: lte.c (nRF91
 *  modem), wifi.c (ESP32-C6 station) or native_sim.c (host sockets).
 */
#ifndef NET_CONNECT_H
#define NET_CONNECT_H

#include <stddef.h>

/** Brings the interface up and blocks until it has connectivity. */
int net_connect(void);

/** Takes the interface down; on a modem this includes the graceful power-off. */
int net_disconnect(void);

/** Installs a PEM CA certificate under sec_tag for this board's TLS stack.
 *  Call it before net_connect(): a modem's store only accepts writes offline. */
int net_install_ca(int sec_tag, const char* pem, size_t len);

#endif
