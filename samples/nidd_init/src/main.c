#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "shadow.h"

LOG_MODULE_REGISTER(main);

int main(void) {
  /* The modem's IMEI names the pigeon on NIDD, so device_id only labels log lines. */
  struct pigeon_config config = {
      .device_id = "nidd_init",
      .connector = {.type = PIGEON_CONNECTOR_NIDD},
  };

  int err = nrf_modem_lib_init();

  if (err) {
    LOG_ERR("nrf_modem_lib_init() failed: %d", err);
    return 0;
  }

  /* Before the attach: the Non-IP context must exist by then, and PSM and eDRX are written so
   * the attach request carries them. */
  err = pigeon_init(&config);
  if (err) {
    goto off;
  }

  LOG_INF("Attaching over NB-IoT");
  err = lte_lc_connect();
  if (err) {
    LOG_ERR("lte_lc_connect() failed: %d", err);
    goto off;
  }

  err = pigeon_nidd_start(shadow_event_cb);
  if (err) {
    LOG_ERR("pigeon_nidd_start() failed: %d", err);
    goto off;
  }

  shadow_loop();

  pigeon_nidd_stop();

off:
  /* Powered off rather than reset: the modem refuses to attach for 30 minutes after repeated
   * ungraceful resets. */
  (void)lte_lc_power_off();

  return 0;
}
