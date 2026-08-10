/** @headerfile connection_manager.h
 *
 * native_sim's answer to connection_manager.c: there is no modem to bring
 * up (and, under this workspace's default vanilla-Zephyr manifest, no NCS
 * modem/lte_lc.h to even compile against), so this file swaps the LTE
 * bring-up for a generic conn_mgr flow over NSOS's connectivity-sim
 * interface (CONFIG_NET_NATIVE_OFFLOADED_SOCKETS_CONNECTIVITY_SIM -- see
 * this sample's boards/native_sim_native_64.conf) -- the same
 * board-conditional-source pattern wifi_init/ws_init use for their WiFi
 * bring-up (task #54). CMakeLists.txt selects this file instead of
 * connection_manager.c when CONFIG_BOARD_NATIVE_SIM is set, so pigeon's
 * CoAP/DTLS transport code and everything above this layer (main.c,
 * shadow.c) is untouched -- only network bring-up differs, exactly the
 * point of a native_sim variant: it exercises the platform protocol, not
 * LTE. lte_connect()/lte_disconnect() keep the same names/signatures as
 * the real file so callers don't need a board-specific #ifdef of their
 * own. No credential provisioning happens here: the CoAP connector's PSK
 * registration lives in pigeon itself (pigeon_init()'s config), unlike
 * https_init's CA-cert flow.
 */
#include "connection_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>

#define NATIVE_SIM_CONNECT_TIMEOUT K_SECONDS(30)

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
      LOG_INF("Network connectivity established");
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

  int err = conn_mgr_all_if_up(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  /* The NSOS connectivity-sim interface never runs DHCP and has no L2 of
   * its own to assign an address, but conn_mgr's L4 state machine only
   * fires NET_EVENT_L4_CONNECTED once the interface is BOTH "connected"
   * and carries an address (confirmed against Zephyr's own
   * tests/net/conn_mgr_nsos -- see coap_tcp_init's connection_manager.c,
   * where this dummy-address pattern originates in this repo). Never used
   * for real routing: NSOS offloaded sockets bypass Zephyr's IP stack
   * entirely. */
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

  /* native_sim brings its interface up at SYS_INIT(), before the event
   * handlers above are registered, so the L4_CONNECTED event that would
   * normally give network_connection_sem is missed; re-request it here. */
  conn_mgr_mon_resend_status();

  err = k_sem_take(&network_connection_sem, NATIVE_SIM_CONNECT_TIMEOUT);
  if (err) {
    LOG_ERR("Timed out waiting for network connectivity: %d", err);
    return err;
  }

  return 0;
}

int lte_disconnect(void) {
  int err = conn_mgr_all_if_disconnect(true);

  if (err) {
    LOG_ERR("conn_mgr_all_if_disconnect, error: %d", err);
  }

  err = conn_mgr_all_if_down(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_down, error: %d", err);
  }

  /* No modem to power off here -- graceful CFUN=0 shutdown only matters
   * for the reset-loop protection on real nRF91 hardware (see this repo's
   * CLAUDE.md "Modem reset safety" note and connection_manager.c). */
  return err;
}
