/** @headerfile net_connect.h
 *  native_sim: the host's sockets through NSOS, TLS in mbedTLS.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/tls_credentials.h>

#include "net_connect.h"

LOG_MODULE_DECLARE(net_connect);

#define CONNECT_TIMEOUT K_SECONDS(30)

int net_prepare(void) {
  /* Credentials go into Zephyr's own store, which is ready from boot. */
  return 0;
}

int net_install_ca(int sec_tag, const char* pem, size_t len) {
  int err = tls_credential_add(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE, pem, len);

  if (err == -EEXIST) {
    LOG_INF("CA certificate already installed, sec_tag %d", sec_tag);
    return 0;
  }

  if (err < 0) {
    LOG_ERR("tls_credential_add, error: %d", err);
  }

  return err;
}

int net_connect(void) {
  net_events_arm();

  LOG_INF("Bringing network interface up");

  int err = conn_mgr_all_if_up(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  /* The simulated interface never gets an address of its own, and the
   * connection manager only reports L4 connected once it has one. Offloaded
   * sockets never route through it. */
  struct net_if* iface = net_if_get_default();
  struct in_addr dummy_addr;

  net_addr_pton(AF_INET, "192.0.2.1", &dummy_addr);
  net_if_ipv4_addr_add(iface, &dummy_addr, NET_ADDR_MANUAL, 0);

  LOG_INF("Connecting to the network");

  err = conn_mgr_all_if_connect(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
    return err;
  }

  /* The interface came up at boot, before the handlers above existed. */
  conn_mgr_mon_resend_status();

  err = k_sem_take(&network_connection_sem, CONNECT_TIMEOUT);
  if (err) {
    LOG_ERR("Timed out waiting for the network: %d", err);
    return err;
  }

  return 0;
}

int net_disconnect(void) {
  k_sleep(K_SECONDS(1));

  int err = conn_mgr_all_if_disconnect(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_disconnect, error: %d", err);
  }

  int down_err = conn_mgr_all_if_down(true);

  if (down_err) {
    LOG_ERR("conn_mgr_all_if_down, error: %d", down_err);
  }

  return err ? err : down_err;
}
