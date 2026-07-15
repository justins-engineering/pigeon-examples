#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net/connection_manager.h"
#include "shadow.h"

int main(void) {
  int err = lte_connect();
  if (err) {
    return err;
  }

  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.local.conf) instead of this struct. device_id must match the
   * endpoint's device path segment (dovecote's get_shadow_device verifies
   * the bearer token against this pigeon's own stored device_public_key,
   * not against any claim in the token itself -- there's no JWT anymore,
   * see pigeon's CLAUDE.md), even though it isn't used to build the shadow
   * request itself -- pigeon_shadow_get() relies solely on
   * CONFIG_PIGEON_ENDPOINT for that. */
  struct pigeon_config config = {
      .device_id = "1da98d5413f5bc79e03bf5648aa976d85a6634e3e05b1a91c85c79f7adb9d821",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  err = pigeon_init(&config);
  if (err) {
    lte_disconnect();
    return err;
  }

  /* pigeon_set_shadow_param() (queuing local metrics/state to push to the
   * platform) is declared in pigeon.h but not yet implemented, so it's not
   * called here -- only the platform -> device direction (shadow_loop) is
   * wired up so far. shadow_loop() polls forever (interval driven by the
   * shadow's own telemetry_interval field), matching a normally-connected
   * device rather than this sample's original one-shot connect/disconnect;
   * it does not return under normal operation. */
  shadow_loop();

  return lte_disconnect();
}
