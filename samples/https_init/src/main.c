#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net/connection_manager.h"

int main(void) {
  int err = lte_connect();
  if (err) {
    return err;
  }

  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.conf) instead of this struct. */
  struct pigeon_config config = {
      .device_id = "demo-pigeon-0001",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  err = pigeon_init(&config);
  if (err) {
    lte_disconnect();
    return err;
  }

  /* pigeon_set_shadow_param() is declared in pigeon.h but not yet
   * implemented (see pigeon's CLAUDE.md), so it's not called here. */

  return lte_disconnect();
}
