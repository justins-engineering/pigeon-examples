#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net/connection_manager.h"
#include "shadow.h"

/*
 * CoAP over DTLS/UDP (RFC 7252 coaps://) -- pigeon's
 * CONFIG_PIGEON_COAP_TRANSPORT_UDP, the primary CoAP transport for
 * constrained battery/cellular devices: one long-lived PSK DTLS session
 * (optionally with RFC 9146 Connection ID, CONFIG_PIGEON_COAP_DTLS_CID, so
 * it survives NAT rebinds/PSM sleeps) carrying confirmable exchanges with
 * real retransmission/dedup, instead of coap_tcp_init's
 * TLS-handshake-per-exchange stream transport. The real dovecote backend
 * still has no CoAP listener deployed (the terminator is a tracked roadmap
 * item); this sample's protocol conformance is verified against libcoap's
 * coap-server instead -- see this repo's README.
 */
int main(void) {
  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.local.conf). tls_psk_identity/secret are placeholders here --
   * pigeon registers them under CONFIG_PIGEON_COAP_SEC_TAG at
   * pigeon_init() time. */
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0003",
      .connector =
          {
              .type = PIGEON_CONNECTOR_COAP,
              .coap =
                  {
                      .tls_psk_identity = "demo-pigeon-0003",
                      .tls_psk_secret = "replace-with-device-token",
                  },
          },
  };

  /* Unlike coap_tcp_init, pigeon_init() runs BEFORE LTE comes up: on
   * modem-offloaded boards (CONFIG_MODEM_KEY_MGMT) it writes the PSK into
   * the modem's own credential store, which only accepts writes while the
   * modem is offline -- see pigeon_coap_psk_write_modem() in ~/pigeon.
   * Harmless on boards without a modem store (native TLS registration
   * doesn't care about ordering). */
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
