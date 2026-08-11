#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net/connection_manager.h"
#include "shadow.h"

/*
 * CoAP over DTLS/UDP (RFC 7252 coaps://) -- pigeon's
 * CONFIG_PIGEON_COAP_TRANSPORT_UDP, the primary CoAP transport for
 * constrained battery/cellular devices: one long-lived PSK DTLS session
 * carrying confirmable exchanges with real retransmission/dedup, instead
 * of coap_tcp_init's TLS-handshake-per-exchange stream transport. The PSK
 * handshake is the device's entire authentication -- the platform maps
 * the PSK identity to the pigeon and holds the bearer token server-side,
 * so unlike the HTTPS samples no CONFIG_PIGEON_TOKEN is needed here.
 */

/* A Kconfig string is always defined, so "" is its only way of saying "not
 * supplied" -- map that to NULL so pigeon_init() takes its documented
 * absent-PSK path (skip registration; the app owns whatever credential
 * lives under CONFIG_PIGEON_COAP_SEC_TAG) instead of registering a
 * zero-length credential that fails every handshake. */
#define PSK_CONF_OR_NULL(s) ((s)[0] ? (s) : NULL)

int main(void) {
  /* The endpoint comes from CONFIG_PIGEON_ENDPOINT; PSK identity/secret
   * from CONFIG_PIGEON_COAP_TLS_PSK_IDENTITY/_SECRET (see prj.local.conf)
   * -- real values never belong as literals in tracked source. pigeon
   * registers whatever the app supplies here under
   * CONFIG_PIGEON_COAP_SEC_TAG at pigeon_init() time. device_id is
   * log-only (see https_init's main.c for why), left as a neutral
   * placeholder for the same reason the endpoint/PSK aren't. */
  struct pigeon_config config = {
      .device_id = "pigeon-coap-dtls-sample",
      .connector =
          {
              .type = PIGEON_CONNECTOR_COAP,
              .coap =
                  {
                      .tls_psk_identity = PSK_CONF_OR_NULL(CONFIG_PIGEON_COAP_TLS_PSK_IDENTITY),
                      .tls_psk_secret = PSK_CONF_OR_NULL(CONFIG_PIGEON_COAP_TLS_PSK_SECRET),
                  },
          },
  };

  /* Unlike coap_tcp_init, pigeon_init() runs BEFORE the network comes up:
   * on modem-offloaded boards (CONFIG_MODEM_KEY_MGMT) it writes the PSK
   * into the modem's own credential store, which only accepts writes
   * while the modem is offline. Harmless everywhere else -- native TLS
   * credential registration (ESP32-C6/native_sim) doesn't care about
   * ordering -- so this order is kept as the one that works for every
   * board this sample targets rather than branching on it. */
  int err = pigeon_init(&config);

  if (err) {
    return err;
  }

  err = lte_connect();
  if (err) {
    return err;
  }

  /* See https_init's main.c: only the platform -> device direction is
   * exercised as a poll loop; shadow_loop() does not return under normal
   * operation. */
  shadow_loop();

  return lte_disconnect();
}
