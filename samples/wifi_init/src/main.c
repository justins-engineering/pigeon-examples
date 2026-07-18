#include <pigeon.h>
#include <zephyr/kernel.h>

#include "net/wifi_connection_manager.h"
#include "shadow.h"

int main(void) {
  int err = wifi_connect();
  if (err) {
    return err;
  }

  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.local.conf) instead of this struct -- same convention as
   * https_init's main.c. device_id is log-only, see the comment there for
   * why. */
  struct pigeon_config config = {
      .device_id = "pigeon-wifi-sample",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  err = pigeon_init(&config);
  if (err) {
    wifi_disconnect();
    return err;
  }

  /* shadow_loop() polls forever; it does not return under normal
   * operation. */
  shadow_loop();

  return wifi_disconnect();
}
