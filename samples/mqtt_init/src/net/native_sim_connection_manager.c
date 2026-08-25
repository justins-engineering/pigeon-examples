/** @headerfile wifi_connection_manager.h
 *
 * native_sim's answer to wifi_connection_manager.c: there is no real WiFi
 * radio to join a network with, so this file swaps the ESP32 WiFi bring-up
 * for a generic conn_mgr flow over NSOS's connectivity-sim interface
 * instead (CONFIG_NET_NATIVE_OFFLOADED_SOCKETS_CONNECTIVITY_SIM -- see this
 * sample's boards/native_sim_native_64.conf) -- the same pattern
 * https_init/coap_tcp_init's connection_manager.c already uses for LTE.
 * CMakeLists.txt selects this file instead of wifi_connection_manager.c
 * when CONFIG_BOARD_NATIVE_SIM is set, so pigeon's MQTT connector and
 * everything above this layer (main.c, shadow.c) is untouched -- only
 * network bring-up differs, exactly the point of a native_sim variant: it
 * exercises the platform protocol, not WiFi. wifi_connect()/wifi_disconnect()
 * keep the same names/signatures as the real file so callers don't need a
 * board-specific #ifdef of their own.
 */
#include "wifi_connection_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/tls_credentials.h>

#define NATIVE_SIM_CONNECT_TIMEOUT K_SECONDS(30)

LOG_MODULE_REGISTER(wifi_connection_manager);

#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
/*
 * The trust anchor a certificate-authenticated MQTT session verifies the
 * broker against. Which certificate this is comes from CMakeLists.txt's
 * PIGEON_MQTT_CA_FILE, defaulting to cert/isrg-root-x2.pem: the platform's
 * broker is issued with certbot's `--key-type ecdsa --preferred-chain
 * "ISRG Root X2"`, so the served chain is a P-256 leaf under an ECDSA
 * intermediate anchored at ISRG Root X2 (itself P-384) -- all-ECDSA, which
 * is why this build wants no RSA verification at all.
 *
 * The default ECDSA chain would instead end at X2 cross-signed BY X1, an
 * RSA-4096 signature, and pinning that form is what would drag RSA back
 * into a constrained build. Anchoring the self-signed X2 removes it:
 * mbedTLS matches a child certificate's issuer to a trusted root by subject
 * plus public key, not by which of two co-existing certificates for that
 * identity established the trust, so the intermediate validates against
 * this root either way.
 *
 * A local broker with its own development CA is pointed at that file
 * instead (scripts/test/native-sim-e2e.sh does exactly that), which is why
 * the path is a build knob rather than a hardcoded include.
 */
static const char broker_ca_cert[] = {
#include "broker-ca.pem.hex"
    /* Null terminate certificate if running Mbed TLS */
    IF_ENABLED(CONFIG_TLS_CREDENTIALS, (0x00))
};
#endif /* CONFIG_PIGEON_MQTT_AUTH_CERT */

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

K_SEM_DEFINE(network_connection_sem, 0, 1);

static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
/* Provisions the broker's trust anchor under the sec_tag pigeon's MQTT
 * connector names (CONFIG_PIGEON_MQTT_SEC_TAG). A PSK build provisions
 * nothing here: pigeon registers the identity and secret itself from
 * pigeon_init()'s config, and there is no certificate to verify. */
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
#endif /* CONFIG_PIGEON_MQTT_AUTH_CERT */

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
  }
}

int wifi_connect(void) {
  int err;

  net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
  net_mgmt_add_event_callback(&l4_cb);

  net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
  net_mgmt_add_event_callback(&conn_cb);

#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
  /* Provisioned before the link comes up: the session's first TLS handshake
   * would otherwise have no CA to verify the broker against. */
  err = provision_cert(BROKER_SEC_TAG, broker_ca_cert, sizeof(broker_ca_cert));
  if (err) {
    LOG_ERR("Failed to provision TLS certificate. sec_tag: %d", BROKER_SEC_TAG);
    return err;
  }
#endif /* CONFIG_PIGEON_MQTT_AUTH_CERT */

  LOG_INF("Bringing network interface up");

  err = conn_mgr_all_if_up(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  /* The NSOS connectivity-sim interface never runs DHCP and has no L2 of
   * its own to assign an address, but conn_mgr's L4 state machine only
   * fires NET_EVENT_L4_CONNECTED once the interface is both "connected"
   * and carries an address -- the same technique Zephyr's own
   * tests/net/conn_mgr_nsos uses before triggering its own connect.
   * Without it wifi_connect() below hangs until NATIVE_SIM_CONNECT_TIMEOUT
   * even though NSOS's own socket layer comes up fine. Never used for real
   * routing -- NSOS offloaded sockets bypass Zephyr's IP stack entirely --
   * this exists purely to satisfy conn_mgr's gating check, same technique
   * coap_tcp_init/connection_manager.c uses. */
  struct net_if *iface = net_if_get_default();
  struct in_addr dummy_addr;

  net_addr_pton(AF_INET, "192.0.2.1", &dummy_addr);
  net_if_ipv4_addr_add(iface, &dummy_addr, NET_ADDR_MANUAL, 0);

  LOG_INF("Connecting to the network");

  err = conn_mgr_all_if_connect(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
    return err;
  }

  err = k_sem_take(&network_connection_sem, NATIVE_SIM_CONNECT_TIMEOUT);
  if (err) {
    LOG_ERR("Timed out waiting for network connectivity: %d", err);
    return err;
  }

  return 0;
}

int wifi_disconnect(void) {
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
