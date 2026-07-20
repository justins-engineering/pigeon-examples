#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net/wifi_connection_manager.h"
#include "shadow.h"

#if defined(CONFIG_PIGEON_WS)
LOG_MODULE_REGISTER(main);
#endif

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

#if defined(CONFIG_PIGEON_WS)
  /* Persistent push channel alongside the HTTPS connector above (not a
   * replacement for it -- see pigeon's zephyr/Kconfig: CONFIG_PIGEON_WS
   * depends on CONFIG_PIGEON_CONNECTOR_HTTPS). A failure here just means
   * shadow_loop() falls back to plain polling for this boot; it isn't
   * fatal to startup. */
  err = pigeon_ws_start(shadow_ws_event_cb);
  if (err) {
    LOG_WRN("pigeon_ws_start() failed: %d (falling back to HTTPS polling)", err);
  }
#endif

  /* shadow_loop() polls forever; it does not return under normal
   * operation. */
  shadow_loop();

  return wifi_disconnect();
}
