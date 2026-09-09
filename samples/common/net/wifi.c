/** @headerfile net_connect.h
 *  ESP32-C6: WiFi station with static credentials, TLS in mbedTLS.
 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/wifi_mgmt.h>

#include "net_connect.h"

LOG_MODULE_REGISTER(net_connect);

#define CONNECT_TIMEOUT K_SECONDS(30)

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
      LOG_INF("Network connected, address assigned");
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
  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);
  net_mgmt_init_event_callback(&conn_cb, conn_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);

  LOG_INF("Bringing WiFi interface up");

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

  /* A join can time out on one boot and succeed on the next, and a headless
   * device has nobody to retry it, so a failed attempt is retried forever. */
  for (int attempt = 1;; attempt++) {
    /* The WiFi driver has no connection manager binding, so the join itself
     * is requested explicitly below. */
    err = conn_mgr_all_if_connect(true);
    if (err) {
      LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
      return err;
    }

    LOG_INF("Joining stored WiFi network (attempt %d)", attempt);

    err = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, wifi_iface, NULL, 0);
    if (err) {
      LOG_ERR("Attempt %d: NET_REQUEST_WIFI_CONNECT_STORED, error: %d", attempt, err);
    } else {
      err = k_sem_take(&connected_sem, CONNECT_TIMEOUT);
      if (err) {
        LOG_ERR("Attempt %d: timed out waiting for the network: %d", attempt, err);
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

int net_disconnect(void) {
  /* Lets an open TCP connection finish closing first. */
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
