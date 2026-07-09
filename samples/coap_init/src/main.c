#include <pigeon.h>
#include <zephyr/kernel.h>

int main(void) {
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0002",
      .connector =
          {
              .type = PIGEON_CONNECTOR_COAP,
              .cfg.coap =
                  {
                      .endpoint = "coaps://api.pidgeiot.com/device/pigeons/demo-pigeon-0002",
                      .token = "replace-with-device-jwt",
                      .dtls_psk_identity = "demo-pigeon-0002",
                      .dtls_psk_secret = "replace-with-device-jwt",
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
