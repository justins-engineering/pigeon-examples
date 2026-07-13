#include <pigeon.h>
#include <zephyr/kernel.h>

/*
 * CoAP over TLS/TCP (RFC 8323 coaps+tcp://), not the usual CoAP-over-DTLS/UDP:
 * this device stack has no UDP support yet. See pigeon's CLAUDE.md "Known
 * wire-compat gap" note — the real backend only serves coaps:// (UDP/DTLS)
 * as of this writing.
 */
int main(void) {
  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.conf) instead of this struct. */
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

  int err = pigeon_init(&config);
  if (err) {
    return err;
  }

  /* pigeon_set_shadow_param() is declared in pigeon.h but not yet
   * implemented (see pigeon's CLAUDE.md), so it's not called here. */

  return 0;
}
