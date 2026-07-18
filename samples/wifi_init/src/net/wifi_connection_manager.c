/** @headerfile wifi_connection_manager.h */
#include "wifi_connection_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/tls_credentials.h>

#define WIFI_CONNECT_TIMEOUT K_SECONDS(30)

LOG_MODULE_REGISTER(wifi_connection_manager);

static const char ca_cert[] = {
#include "GTS_Root_R4.crt.hex"
    // Null terminate certificate if running Mbed TLS
    IF_ENABLED(CONFIG_TLS_CREDENTIALS, (0x00))
};

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

/* Starts at 0: wifi_connect() blocks on this until on_net_event_l4_connected()
 * gives it, so it must not be pre-satisfied before the connect is requested. */
K_SEM_DEFINE(network_connection_sem, 0, 1);

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

/* Provision the CA cert used for pigeon's HTTPS connector. Unlike
 * connection_manager.c's provision_cert() (https_init/coap_tcp_init), there
 * is no CONFIG_MODEM_KEY_MGMT branch here: ESP32-C6 has no cellular modem to
 * offload TLS sockets to, so tls_credential_add() is the only path,
 * unconditionally. */
static int provision_cert(int sec_tag, const char cert[], size_t cert_len) {
  int err = tls_credential_add(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE, cert, cert_len);

  if (err == -EEXIST) {
    LOG_INF("CA certificate already exists, sec tag: %d", sec_tag);
  } else if (err < 0) {
    LOG_ERR("Failed to register CA certificate: %d", err);
    return err;
  }

  return 0;
}

static void on_net_event_l4_disconnected(void) { LOG_INF("Disconnected from the network"); }

static void on_net_event_l4_connected(void) { k_sem_give(&network_connection_sem); }

static void l4_event_handler(
    struct net_mgmt_event_callback* cb, uint64_t event, struct net_if* iface
) {
  switch (event) {
    case NET_EVENT_L4_CONNECTED:
      LOG_INF("Network connectivity established and IP address assigned");
      on_net_event_l4_connected();
      break;
    case NET_EVENT_L4_DISCONNECTED:
      LOG_WRN("Network connectivity lost");
      on_net_event_l4_disconnected();
      break;
    default:
      break;
  }
}

static void connectivity_event_handler(
    struct net_mgmt_event_callback* cb, uint64_t event, struct net_if* iface
) {
  if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
    LOG_ERR("Fatal error received from the connectivity layer");
    return;
  }
}

int wifi_connect(void) {
  int err;

  /* Setup handler for Zephyr NET Connection Manager events. */
  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);

  /* Setup handler for Zephyr NET Connection Manager Connectivity layer. */
  net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);

  /* Provision the CA cert before connecting -- pigeon_https.c's first
   * request would otherwise fail the TLS handshake with no CA to verify
   * against. */
  err = provision_cert(WIFI_SEC_TAG, ca_cert, sizeof(ca_cert));
  if (err) {
    LOG_ERR("Failed to provision TLS certificate. sec_tag: %d", WIFI_SEC_TAG);
    return err;
  }

  LOG_INF("Bringing WiFi interface up");

  /* conn_mgr_all_if_up/connect() are the same generic connectivity-layer
   * calls connection_manager.c's lte_connect() makes -- the ESP32 WiFi
   * driver's CONFIG_WIFI_CREDENTIALS_STATIC-backed connectivity binding
   * (wifi_mgmt.c's add_static_network_config(), see this sample's prj.conf)
   * is what actually answers "connect" here instead of an LTE modem. */
  err = conn_mgr_all_if_up(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  err = conn_mgr_all_if_connect(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
    return err;
  }

  /* Bounded, not K_FOREVER: give up rather than hang forever if the AP is
   * out of range or credentials are wrong -- mirrors
   * connection_manager.c's lte_connect() reasoning, minus the modem
   * reset-loop-protection consequence (there's no modem here). */
  err = k_sem_take(&network_connection_sem, WIFI_CONNECT_TIMEOUT);
  if (err) {
    LOG_ERR("Timed out waiting for network connectivity: %d", err);
    wifi_disconnect();
    return err;
  }

  return 0;
}

int wifi_disconnect(void) {
  int err;

  /* A small delay for the TCP connection teardown */
  k_sleep(K_SECONDS(1));

  err = conn_mgr_all_if_disconnect(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_disconnect, error: %d", err);
  }

  int down_err = conn_mgr_all_if_down(true);

  if (down_err) {
    LOG_ERR("conn_mgr_all_if_down, error: %d", down_err);
  }

  return err ? err : down_err;
}
