/** @headerfile net_connect.h
 *  nRF91: the modem owns LTE, TLS and the credential store.
 */
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include "net_connect.h"

LOG_MODULE_REGISTER(net_connect);

/* Bounded so a modem that cannot attach is powered off rather than reset: the
 * modem refuses to attach for 30 minutes after repeated ungraceful resets. */
#define CONNECT_TIMEOUT K_SECONDS(120)

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

static K_SEM_DEFINE(connected_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

static void l4_event_handler(
    struct net_mgmt_event_callback* cb, uint64_t event, struct net_if* iface
) {
  switch (event) {
    case NET_EVENT_L4_CONNECTED:
      LOG_INF("Network connected");
      k_sem_give(&connected_sem);
      break;
    case NET_EVENT_L4_DISCONNECTED:
      LOG_WRN("Network disconnected");
      break;
    default:
      break;
  }
}

static void conn_event_handler(
    struct net_mgmt_event_callback* cb, uint64_t event, struct net_if* iface
) {
  if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
    LOG_ERR("Fatal error from the connectivity layer");
  }
}

int net_prepare(void) {
  /* The interface brings the library up later; nothing else does it earlier,
   * and by then the store no longer accepts writes. */
  if (nrf_modem_is_initialized()) {
    return 0;
  }

  int err = nrf_modem_lib_init();

  if (err) {
    LOG_ERR("nrf_modem_lib_init, error: %d", err);
  }

  return err;
}

int net_install_ca(int sec_tag, const char* pem, size_t len) {
  int err = net_prepare();

  if (err) {
    return err;
  }

  /* A tag holds either a certificate or a PSK; one left here by an earlier
   * build fails every handshake as a connect error, with no credential error
   * to point at it. */
  (void)modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_IDENTITY);
  (void)modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_PSK);

  bool exists;
  err = modem_key_mgmt_exists(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);

  if (err) {
    LOG_ERR("modem_key_mgmt_exists, error: %d", err);
    return err;
  }

  if (exists) {
    if (modem_key_mgmt_cmp(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, pem, len) == 0) {
      LOG_INF("CA certificate already provisioned, sec_tag %d", sec_tag);
      return 0;
    }

    err = modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
    if (err) {
      LOG_WRN("modem_key_mgmt_delete, error: %d", err);
    }
  }

  LOG_INF("Provisioning CA certificate, sec_tag %d", sec_tag);

  err = modem_key_mgmt_write(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, pem, len);
  if (err) {
    LOG_ERR("modem_key_mgmt_write, error: %d", err);
  }

  return err;
}

int net_connect(void) {
  /* A connect that lands after the previous attempt timed out would otherwise
   * satisfy this one against an interface that is already going down. */
  k_sem_reset(&connected_sem);

  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);
  net_mgmt_init_event_callback(&conn_cb, conn_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);

  LOG_INF("Bringing network interface up");

  int err = conn_mgr_all_if_up(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  LOG_INF("Connecting to the network");

  err = conn_mgr_all_if_connect(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
    return err;
  }

  err = k_sem_take(&connected_sem, CONNECT_TIMEOUT);
  if (err) {
    LOG_ERR("Timed out waiting for the network: %d", err);
    net_disconnect();
    return err;
  }

  return 0;
}

int net_disconnect(void) {
  /* Lets an open TCP connection finish closing first. */
  k_sleep(K_SECONDS(1));

  int err = conn_mgr_all_if_disconnect(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_disconnect, error: %d", err);
  }

  err = conn_mgr_all_if_down(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_down, error: %d", err);
  }

  /* Interface-down only deactivates LTE (CFUN=20); the modem counts only a
   * confirmed CFUN=0 as a graceful shutdown, so this is retried, not logged. */
  LOG_INF("Powering off modem");

  int power_off_err = -EAGAIN;

  for (int attempt = 1; attempt <= 3 && power_off_err; attempt++) {
    /* Gives the previous CFUN command time to complete. */
    k_sleep(K_SECONDS(attempt));

    power_off_err = lte_lc_power_off();
    if (power_off_err) {
      LOG_WRN("lte_lc_power_off attempt %d/3 failed: %d", attempt, power_off_err);
    }
  }

  if (power_off_err) {
    LOG_ERR("lte_lc_power_off, error: %d (modem may not be gracefully off)", power_off_err);
    return err ? err : power_off_err;
  }

  LOG_INF("Modem powered off");

  return err;
}
