#include <pigeon.h>
#include <zephyr/kernel.h>

int main(void) {
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0001",
      .connector =
          {
              .type = PIGEON_CONNECTOR_HTTPS,
              .cfg.https =
                  {
                      .endpoint = "https://api.pidgeiot.com/device/pigeons/demo-pigeon-0001",
                      .token = "replace-with-device-jwt",
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
