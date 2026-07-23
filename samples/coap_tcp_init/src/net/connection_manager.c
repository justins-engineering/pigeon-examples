/** @headerfile connection_manager.h */
#include "connection_manager.h"

#include <modem/lte_lc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>

/* conn_mgr_all_if_down() only sends CFUN=20 (deactivate LTE), which the modem
 * does NOT count as a graceful shutdown for its reset-loop protection (see
 * LTE_LC_MODEM_EVT_RESET_LOOP's doc comment in modem/lte_lc.h). Repeated
 * ungraceful resets (e.g. during development, reflashing without powering
 * off first) make the modem refuse to attach for the next 30 minutes, so
 * lte_disconnect() below explicitly powers off afterward too. Mirrors
 * https_init's connection_manager.c, where this pattern originates -- see
 * this repo's CLAUDE.md "Modem reset safety" note. */
#define LTE_CONNECT_TIMEOUT K_SECONDS(120)

LOG_MODULE_REGISTER(connection_manager);

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

/* Starts at 0: lte_connect() blocks on this until on_net_event_l4_connected()
 * gives it, so it must not be pre-satisfied before the connect is requested. */
K_SEM_DEFINE(network_connection_sem, 0, 1);

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

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

int lte_connect(void) {
  int err;

  /* Setup handler for Zephyr NET Connection Manager events. */
  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);

  /* Setup handler for Zephyr NET Connection Manager Connectivity layer. */
  net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);

  LOG_INF("Bringing network interface up");

  /* Connecting to the configured connectivity layer. PSK credentials for the
   * CoAP-over-TLS/TCP connector are registered by pigeon_coap.c itself (from
   * pigeon_init()'s config), not provisioned here -- unlike https_init, this
   * sample has no CA cert to provision up front. */
  err = conn_mgr_all_if_up(true);
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

  /* native_sim brings its interface up at SYS_INIT(), before the event
   * handlers above are registered, so the L4_CONNECTED event that would
   * normally give network_connection_sem is missed; re-request it here. */
  if (IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)) {
    conn_mgr_mon_resend_status();
  }

  /* Bounded, not K_FOREVER: if the network never attaches (no coverage, or
   * the modem's own reset-loop protection is currently restricting attach
   * attempts), give up and power the modem off gracefully instead of hanging
   * forever and forcing an ungraceful external reset. */
  err = k_sem_take(&network_connection_sem, LTE_CONNECT_TIMEOUT);
  if (err) {
    LOG_ERR("Timed out waiting for network connectivity: %d", err);
    lte_disconnect();
    return err;
  }

  return 0;
}

int lte_disconnect(void) {
  int err;

  /* A small delay for the TCP connection teardown */
  k_sleep(K_SECONDS(1));

  err = conn_mgr_all_if_disconnect(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_disconnect, error: %d", err);
  }

  err = conn_mgr_all_if_down(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_down, error: %d", err);
  }

  /* native_sim has no real modem to power off, and CONFIG_LTE_LINK_CONTROL
   * isn't available there -- lte_lc_power_off() wouldn't even link. Graceful
   * CFUN=0 shutdown only matters for the reset-loop protection on real nRF91
   * hardware (see this repo's CLAUDE.md "Modem reset safety" note). */
  if (IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)) {
    return err;
  }

  /* conn_mgr_all_if_down() only sends CFUN=20 (deactivate LTE); explicitly
   * power off (CFUN=0) so the modem counts this as a graceful shutdown and
   * doesn't arm its reset-loop protection on the next boot. Only a
   * successful CFUN=0 counts, so this is retried rather than logged and
   * ignored on failure. */
  LOG_INF("Powering off modem");

  int power_off_err = -EAGAIN;

  for (int attempt = 1; attempt <= 3 && power_off_err; attempt++) {
    /* CFUN=20 above may still be completing asynchronously; give the AT
     * command queue time to clear before the next CFUN request, and back
     * off further between retries. */
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

  LOG_INF("Modem powered off gracefully (CFUN=0 confirmed)");

  return err;
}
