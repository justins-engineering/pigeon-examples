/** @headerfile wifi_connection_manager.h */
#include "wifi_connection_manager.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/wifi_mgmt.h>

#define WIFI_CONNECT_TIMEOUT K_SECONDS(30)

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
 * RSA-4096 signature, and pinning the cross-signed form is what would drag
 * RSA back into a constrained build. Anchoring the self-signed X2 removes
 * it: mbedTLS matches a child cert's issuer to a trusted root by subject
 * plus public key, not by which of two co-existing certificates for that
 * identity established the trust, so the intermediate validates against
 * this root either way.
 *
 * A local broker with its own development CA is pointed at that file
 * instead (scripts/test/native-sim-e2e.sh does exactly that), which is the
 * whole reason the path is a build knob rather than a hardcoded include.
 */
static const char broker_ca_cert[] = {
#include "broker-ca.pem.hex"
    /* Null terminate certificate if running Mbed TLS */
    IF_ENABLED(CONFIG_TLS_CREDENTIALS, (0x00))
};
#endif /* CONFIG_PIGEON_MQTT_AUTH_CERT */

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

/* Starts at 0: wifi_connect() blocks on this until on_net_event_l4_connected()
 * gives it, so it must not be pre-satisfied before the connect is requested. */
K_SEM_DEFINE(network_connection_sem, 0, 1);

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
/* Provisions the broker's trust anchor under the sec_tag pigeon's MQTT
 * connector names (CONFIG_PIGEON_MQTT_SEC_TAG). A PSK build provisions
 * nothing here: pigeon registers the identity/secret itself from
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

#if defined(CONFIG_PIGEON_MQTT_AUTH_CERT)
  /* Provisioned before connecting: the session's first TLS handshake would
   * otherwise have no CA to verify the broker against. */
  err = provision_cert(BROKER_SEC_TAG, broker_ca_cert, sizeof(broker_ca_cert));
  if (err) {
    LOG_ERR("Failed to provision TLS certificate. sec_tag: %d", BROKER_SEC_TAG);
    return err;
  }
#endif /* CONFIG_PIGEON_MQTT_AUTH_CERT */

  LOG_INF("Bringing WiFi interface up");

  /*
   * conn_mgr_all_if_up() is the same generic connectivity-layer call
   * connection_manager.c's lte_connect() makes, and does bring the
   * interface administratively up -- but unlike the LTE sample, nothing
   * here actually joins a network on its own. conn_mgr_all_if_connect()
   * below is a genuine no-op for WiFi on this vendored tree: it only acts
   * on interfaces bound to a conn_mgr connectivity implementation
   * (conn_mgr_if_is_bound(), see conn_mgr_connectivity.c's
   * conn_mgr_conn_all_if_cb() -- unbound ifaces are silently skipped, not
   * an error), and the only such implementation choice here is
   * CONNECTIVITY_WIFI_MGMT_APPLICATION, which is unset (would need an
   * application-supplied binding this sample never wrote).
   * CONFIG_WIFI_CREDENTIALS_STATIC=y does NOT create that binding either --
   * it's consumed instead by NET_REQUEST_WIFI_CONNECT_STORED (Kconfig
   * default y, zephyr/subsys/net/l2/wifi/wifi_mgmt.c's
   * connect_stored_command()) right after, which loads
   * CONFIG_WIFI_CREDENTIALS_STATIC_SSID/_PASSWORD via
   * add_static_network_config() and issues the real NET_REQUEST_WIFI_CONNECT
   * itself -- that's the actual join mechanism. conn_mgr_all_if_connect()
   * is kept anyway (harmless, and forward-compatible if a future change
   * adds a real conn_mgr WiFi binding).
   */
  err = conn_mgr_all_if_up(true);
  if (err) {
    LOG_ERR("conn_mgr_all_if_up, error: %d", err);
    return err;
  }

  struct net_if *wifi_iface = net_if_get_first_wifi();

  if (!wifi_iface) {
    LOG_ERR("No WiFi interface found");
    return -ENODEV;
  }

  /*
   * Retried indefinitely rather than surfaced as a wifi_connect() failure
   * after one attempt: WiFi join here is genuinely flaky in practice --
   * alternating ~3-4s join success and full WIFI_CONNECT_TIMEOUT (30s)
   * timeouts across otherwise identical boots, cause undiagnosed (AP-side
   * or driver race). Surfacing a single failed join as a hard failure
   * would leave the board dead until a physical reset -- wrong for a
   * headless/unattended device sample, which has no operator to retry it
   * and must keep converging on its own.
   *
   * conn_mgr_all_if_up() above deliberately stays outside this loop:
   * net_if_up() (see conn_mgr_conn_all_if_cb()'s ALL_IF_UP case) is already
   * a no-op once the interface is admin-up, so nothing is gained by
   * re-issuing it every attempt. Only the connect request + wait is
   * retried -- neither is torn down with wifi_disconnect()/conn_mgr_all_if_
   * down() between attempts first, since that would also take the
   * interface back down (unlike conn_mgr_all_if_connect(), which is a pure
   * no-op here, see the comment above) and require re-upping it before the
   * next NET_REQUEST_WIFI_CONNECT_STORED; simply re-requesting the connect
   * is enough to observe the driver recovering on a later attempt.
   */
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
      /* Bounded, not K_FOREVER: give up on *this attempt* rather than hang
       * forever if the AP is out of range or credentials are wrong --
       * mirrors connection_manager.c's lte_connect() reasoning, minus the
       * modem reset-loop-protection consequence (there's no modem here).
       * A timeout doesn't end wifi_connect() itself here, though -- see
       * this loop's header comment. */
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
