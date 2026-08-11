#include <pigeon.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "gnss.h"
#include "net/connection_manager.h"
#include "shadow.h"

LOG_MODULE_REGISTER(main);

/* A boot that hits the nRF91 modem's 30-minute reset-loop restriction
 * (see this repo's CLAUDE.md "Modem reset safety") makes lte_connect()
 * fail/time out exactly once. Returning that error straight out of
 * main() with nothing retrying it would leave the device fully idle (no
 * LTE, no telemetry, no recovery path at all) for whatever's left of the
 * restriction -- a multi-minute-to-multi-hour silent gap that
 * CONFIG_PIGEON_WATCHDOG can't catch, since it only feeds on a successful
 * telemetry report, which never happens if LTE never comes up in the
 * first place. Retrying a bounded number of rounds with backoff (each
 * round already carries lte_connect()'s own internal LTE_CONNECT_TIMEOUT
 * and graceful lte_disconnect() on failure, see connection_manager.c),
 * then self-rebooting is more robust: a fresh boot is often enough to
 * get past a transient failure (a restriction expiring mid-wait, a brief
 * coverage gap), and bounded reboots are still strictly better than
 * silently giving up forever on the very first failed attempt. */
static int lte_connect_with_retry(void) {
  uint32_t backoff_sec = CONFIG_ASSET_TRACKER_LTE_CONNECT_BACKOFF_BASE_SEC;

  for (int round = 1; round <= CONFIG_ASSET_TRACKER_LTE_CONNECT_MAX_ROUNDS; round++) {
    int err = lte_connect();

    if (!err) {
      return 0;
    }

    LOG_ERR(
        "lte_connect() round %d/%d failed: %d", round, CONFIG_ASSET_TRACKER_LTE_CONNECT_MAX_ROUNDS,
        err
    );

    if (round == CONFIG_ASSET_TRACKER_LTE_CONNECT_MAX_ROUNDS) {
      break;
    }

    LOG_WRN("Retrying lte_connect() in %d s", backoff_sec);
    k_sleep(K_SECONDS(backoff_sec));
    backoff_sec = MIN(backoff_sec * 2, CONFIG_ASSET_TRACKER_LTE_CONNECT_BACKOFF_MAX_SEC);
  }

  LOG_ERR(
      "lte_connect() failed all %d rounds; rebooting for a fresh attempt",
      CONFIG_ASSET_TRACKER_LTE_CONNECT_MAX_ROUNDS
  );

  return -ENOTCONN;
}

int main(void) {
  int err = lte_connect_with_retry();

  if (err) {
    sys_reboot(SYS_REBOOT_COLD);
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
