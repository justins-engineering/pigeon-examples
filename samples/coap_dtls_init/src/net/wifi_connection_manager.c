/** @headerfile connection_manager.h
 *
 * ESP32-C6's answer to connection_manager.c (LTE) and
 * native_sim_connection_manager.c (NSOS sim): brings up the board's real
 * WiFi station interface instead, reusing ws_init/wifi_init's join/retry
 * logic (see wifi_connection_manager.c there) rather than re-deriving it.
 * Exports the same lte_connect()/lte_disconnect() names as its siblings
 * so main.c/shadow.c stay untouched -- only network bring-up differs
 * per board.
 *
 * Unlike ws_init/wifi_init's copy of this file, there is no CA cert to
 * provision here: this sample's transport is CoAP-over-DTLS/UDP with a PSK
 * ciphersuite, which authenticates the session with no certificate on
 * either side -- pigeon_init() (called before this, see main.c) already
 * registered the PSK identity/secret under CONFIG_PIGEON_COAP_SEC_TAG via
 * tls_credential_add(), and that is the DTLS stack's entire credential
 * store for this connection.
 */
#include "connection_manager.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#define WIFI_CONNECT_TIMEOUT K_SECONDS(30)

LOG_MODULE_REGISTER(connection_manager);

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

K_SEM_DEFINE(network_connection_sem, 0, 1);

static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

static void l4_event_handler(
    struct net_mgmt_event_callback* cb, uint64_t event, struct net_if* iface
) {
  switch (event) {
    case NET_EVENT_L4_CONNECTED:
      LOG_INF("Network connectivity established and IP address assigned");
      k_sem_give(&network_connection_sem);
      break;
    case NET_EVENT_L4_DISCONNECTED:
      LOG_WRN("Network connectivity lost");
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
  }
}

int lte_connect(void) {
  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);

  net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);

  LOG_INF("Bringing WiFi interface up");

  /* conn_mgr_all_if_connect() below is a genuine no-op for WiFi on this
   * vendored tree (WIFI_ESP32 registers no CONNECTIVITY_WIFI_MGMT_APPLICATION
   * binding) -- kept anyway alongside the explicit join request right
   * after, same reasoning as ws_init/wifi_init's wifi_connection_manager.c. */
  int err = conn_mgr_all_if_up(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  struct net_if* wifi_iface = net_if_get_first_wifi();

  if (!wifi_iface) {
    LOG_ERR("No WiFi interface found");
    return -ENODEV;
  }

  /* Retried indefinitely rather than surfaced as a lte_connect() failure
   * after one attempt -- real hardware has shown the join itself to be
   * flaky across otherwise-identical boots (see ws_init/wifi_init's own
   * note), and this is a headless device with no operator to retry it. */
  for (int attempt = 1;; attempt++) {
    err = conn_mgr_all_if_connect(true);
    if (err) {
      LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
      return err;
    }

    LOG_INF("Requesting connection to stored WiFi credentials (attempt %d)", attempt);

    err = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, wifi_iface, NULL, 0);
    if (err) {
      LOG_ERR("Attempt %d: NET_REQUEST_WIFI_CONNECT_STORED, error: %d", attempt, err);
    } else {
      err = k_sem_take(&network_connection_sem, WIFI_CONNECT_TIMEOUT);
      if (err) {
        LOG_ERR("Attempt %d: timed out waiting for network connectivity: %d", attempt, err);
      }
    }

    if (!err) {
      return 0;
    }

    uint32_t backoff_sec = MIN(5u * (uint32_t)attempt, 30u);

    LOG_WRN("WiFi join attempt %d failed, retrying in %us", attempt, backoff_sec);
    k_sleep(K_SECONDS(backoff_sec));
  }
}

int lte_disconnect(void) {
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
