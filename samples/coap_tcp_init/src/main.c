#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net/connection_manager.h"
#include "shadow.h"

/*
 * CoAP over TLS/TCP (RFC 8323 coaps+tcp://), not the usual CoAP-over-DTLS/UDP:
 * this device stack has no UDP support yet. See pigeon's CLAUDE.md "Known
 * wire-compat gap" note — the real backend only serves coaps:// (UDP/DTLS)
 * as of this writing, and has no CoAP listener at all yet, so shadow_sync()
 * below is expected to fail against the real backend until that lands; the
 * point of this sample is exercising pigeon_coap.c's client-side plumbing.
 */
int main(void) {
  int err = lte_connect();
  if (err) {
    return err;
  }

  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.local.conf). tls_psk_identity/secret are placeholders here --
   * pigeon_coap.c registers them as TLS credentials from this struct at
   * pigeon_init() time (see pigeon's CLAUDE.md). */
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0002",
      .connector =
          {
              .type = PIGEON_CONNECTOR_COAP,
              .coap =
                  {
                      .tls_psk_identity = "demo-pigeon-0002",
                      .tls_psk_secret = "replace-with-device-jwt",
                  },
          },
  };

  err = pigeon_init(&config);
  if (err) {
    lte_disconnect();
    return err;
  }

  /* See https_init's main.c: only the platform -> device direction is
   * exercised as a poll loop; shadow_loop() does not return under normal
   * operation. */
  shadow_loop();

  return lte_disconnect();
}
