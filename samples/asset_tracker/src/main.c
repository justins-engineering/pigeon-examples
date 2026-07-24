#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "gnss.h"
#include "net/connection_manager.h"
#include "shadow.h"

LOG_MODULE_REGISTER(main);

int main(void) {
  int err = lte_connect();

  if (err) {
    return err;
  }

  /* Endpoint and token come from CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN
   * (see prj.local.conf) instead of this struct -- device_id is log-only,
   * see https_init's main.c for the full explanation of why. Left as a
   * neutral placeholder rather than a real pigeon ID: a real pigeon ID
   * belongs only in the gitignored prj.local.conf, never in tracked
   * source. */
  struct pigeon_config config = {
      .device_id = "asset-tracker-sample",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  err = pigeon_init(&config);
  if (err) {
    lte_disconnect();
    return err;
  }

  /* Start GNSS (or, under CONFIG_ASSET_TRACKER_SIM_GPS, the simulated track
   * generator) once LTE is up -- system mode already includes GPS (see
   * CONFIG_LTE_NETWORK_MODE_LTE_M_GPS in prj.conf), so the modem
   * interleaves GNSS search with LTE reception on its own from here on,
   * with nothing more for this app to coordinate. A failure here is logged
   * but not fatal -- shadow_loop() still reports gps_fix_quality=0
   * (TRACKER_FIX_NONE)/gps_sats=0 every poll either way, same as a real
   * "no fix yet" indoors, so the rest of the sample keeps working. */
  err = tracker_gnss_init();
  if (err) {
    LOG_ERR("Failed to start GNSS: %d (continuing without position data)", err);
  }

  /* Runs forever (interval driven by the shadow's own telemetry_interval),
   * reporting uptime + GNSS position telemetry and applying/acking shadow
   * config each poll -- see shadow.c. */
  shadow_loop();

  return lte_disconnect();
}
