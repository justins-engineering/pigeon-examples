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
int main(void) {
  /* The endpoint comes from CONFIG_PIGEON_ENDPOINT (see prj.local.conf).
   * PSK identity is the pigeon's id; the secret is the short key minted
   * alongside the bearer token at provisioning (connector.Coap's
   * tls_psk_identity/tls_psk_secret). Placeholders here -- pigeon
   * registers whatever the app supplies under CONFIG_PIGEON_COAP_SEC_TAG
   * at pigeon_init() time. */
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0003",
      .connector =
          {
              .type = PIGEON_CONNECTOR_COAP,
              .coap =
                  {
                      .tls_psk_identity = "demo-pigeon-0003",
                      .tls_psk_secret = "replace-with-psk-secret",
                  },
          },
  };

  /* Unlike coap_tcp_init, pigeon_init() runs BEFORE LTE comes up: on
   * modem-offloaded boards (CONFIG_MODEM_KEY_MGMT) it writes the PSK into
   * the modem's own credential store, which only accepts writes while the
   * modem is offline. Harmless on boards without a modem store (native
   * TLS registration doesn't care about ordering). */
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
