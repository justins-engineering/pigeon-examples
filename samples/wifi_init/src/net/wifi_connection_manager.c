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

/*
 * Unlike https_init's/coap_tcp_init's identical-content copy of this file,
 * this one intentionally carries only ONE certificate -- the self-signed
 * "GTS Root R4" (ECDSA P-384) -- not Google's full 2-cert published bundle,
 * which also includes a legacy cross-sign of GTS Root R4 by the older RSA
 * "GlobalSign Root CA" (for compatibility with trust stores that don't yet
 * trust the ECC root directly; irrelevant here since this device IS the
 * trust store, being handed GTS Root R4 as its own explicit CA anchor).
 *
 * Root-caused via a native_sim harness with the exact same PSA_WANT_*
 * config this sample builds with (~/pigeon-examples/modules/crypto/mbedtls/
 * library/x509_oid.c:389, gated on PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC):
 * the RSA signature-algorithm OID table entry for "sha256WithRSAEncryption"
 * (the cross-sign cert's own signature algorithm) only compiles in when an
 * RSA *key pair* capability is wanted -- this sample only wants
 * CONFIG_PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY (correct for a pure TLS-client
 * verify-only role), so mbedtls_x509_oid_get_sig_alg() can't recognize
 * that OID and returns MBEDTLS_ERR_X509_UNKNOWN_OID (-0x2100) for the
 * cross-sign cert specifically. mbedtls_x509_crt_parse() then masks this:
 * per its own source (x509_crt.c:1398-1506), if ANY cert in a PEM bundle
 * parses successfully it returns the bare *count* of failed certs (here,
 * 1) instead of the real error -- and Zephyr's tls_add_ca_certificate()
 * (sockets_tls.c) treats any non-zero return as fatal and rejects the
 * whole chain, even though the self-signed cert alone had already parsed
 * fine and was cryptographically sufficient on its own.
 *
 * Dropping the cross-sign is safe for chain validation, not just parsing:
 * mbedTLS matches a child cert's issuer to a trusted root by subject +
 * public key, not by which of two co-existing certs for that identity was
 * used to establish trust -- both certs in Google's bundle embed the
 * identical EC public key (confirmed via `openssl x509 -text` on each),
 * so whatever intermediate (e.g. GTS WE1) actually signs dovecote's leaf
 * cert validates against the self-signed root exactly the same way. The
 * cross-sign only matters for clients relying on a pre-existing OS trust
 * store that already trusts "GlobalSign Root CA" but not yet "GTS Root
 * R4" directly -- not applicable here. Rotation risk: if Google ever
 * rotates GTS Root R4 itself (not just an intermediate), this file needs
 * updating regardless of whether the cross-sign was kept, since a root
 * rotation invalidates both certs in the old bundle simultaneously -- so
 * dropping the cross-sign adds no incremental rotation exposure. The
 * alternative fix (CONFIG_PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT=y) was
 * deliberately not taken: it would make the OID recognizable but asks the
 * PSA crypto layer to want an RSA *key pair* capability this device never
 * uses (verify-only), which is the same over-broad-want problem this
 * sample's PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY choice already avoids
 * elsewhere (see prj.conf).
 */
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
   * application-supplied binding this sample never wrote). The previous
   * version of this comment described a "CONFIG_WIFI_CREDENTIALS_STATIC-
   * backed connectivity binding" answering the connect -- that machinery
   * doesn't exist; the board joined nothing on the first-ever hardware
   * run because of exactly this gap, and the static credentials were
   * never consumed. Kept anyway (harmless, and forward-compatible if a
   * future change adds a real conn_mgr WiFi binding) alongside the actual
   * join request right after: NET_REQUEST_WIFI_CONNECT_STORED (Kconfig
   * default y, zephyr/subsys/net/l2/wifi/wifi_mgmt.c's
   * connect_stored_command()), which -- given
   * CONFIG_WIFI_CREDENTIALS_STATIC=y -- loads
   * CONFIG_WIFI_CREDENTIALS_STATIC_SSID/_PASSWORD via
   * add_static_network_config() and issues the real NET_REQUEST_WIFI_CONNECT
   * itself.
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
   * after one attempt: observed on real hardware (repeated boots, this
   * desk) as genuinely flaky -- alternating ~3-4s join success and full
   * WIFI_CONNECT_TIMEOUT (30s) timeouts across otherwise identical boots,
   * cause undiagnosed (AP-side or driver race). Before this loop, a single
   * failed join left the board dead until a physical reset -- wrong for a
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
       * A timeout no longer ends wifi_connect() itself, though -- see this
       * loop's header comment. */
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
