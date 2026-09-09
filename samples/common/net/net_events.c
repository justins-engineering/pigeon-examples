/** @headerfile net_connect.h
 *  Connectivity events, shared by every board's bring-up.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>

#include "net_connect.h"

LOG_MODULE_REGISTER(net_connect);

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
      LOG_INF("Network connected");
      k_sem_give(&network_connection_sem);
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

void net_events_arm(void) {
  /* A connect that lands after the previous attempt timed out would otherwise
   * satisfy this one against an interface that is already going down. */
  k_sem_reset(&network_connection_sem);

  /* net_mgmt removes a callback before prepending it, so re-arming is safe. */
  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);
  net_mgmt_init_event_callback(&conn_cb, conn_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);
}
