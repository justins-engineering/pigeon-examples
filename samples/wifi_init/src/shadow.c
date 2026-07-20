/** @headerfile shadow.h */
#include "shadow.h"

#include <pigeon.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "net/wifi_connection_manager.h"

LOG_MODULE_REGISTER(shadow);

#if defined(CONFIG_PIGEON_WS)
/* Signaled by shadow_ws_event_cb() (called from the WS worker thread, see
 * pigeon_ws_start()'s docs) to wake shadow_loop() immediately instead of
 * waiting out its telemetry_interval sleep. */
K_SEM_DEFINE(shadow_wakeup, 0, 1);

void shadow_ws_event_cb(enum pigeon_ws_event ev, const struct pigeon_shadow_doc *shadow) {
  ARG_UNUSED(shadow); /* v1: a push is a wakeup, not a data path -- shadow_sync()
                        * always re-fetches over HTTPS rather than consuming the
                        * pushed doc directly (see shadow_loop()'s doc). */

  switch (ev) {
    case PIGEON_WS_EVENT_CONNECTED:
      /* The server sends no state snapshot on accept, so a fresh (or
       * reconnected) socket may mean the platform moved on while we were
       * disconnected -- re-sync now rather than waiting for the next tick. */
    case PIGEON_WS_EVENT_SHADOW_UPDATE:
      k_sem_give(&shadow_wakeup);
      break;
    case PIGEON_WS_EVENT_DISCONNECTED:
      /* Purely informational -- the periodic tick remains the safety net
       * while the socket is down, nothing to wake up for here. */
      break;
  }
}
#endif /* CONFIG_PIGEON_WS */

/* Same struct/decode pattern as https_init's shadow.c, minus the "firmware"
 * key: this sample has no MCUboot/sysbuild setup (see README), so
 * CONFIG_PIGEON_FOTA isn't enabled here and there's nowhere to apply a
 * firmware update to yet even if the shadow requested one. */
struct app_shadow_config {
  bool log;
  int telemetry_interval;
  bool reboot;
};

static const struct json_obj_descr app_shadow_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct app_shadow_config, log, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM(struct app_shadow_config, telemetry_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct app_shadow_config, reboot, JSON_TOK_TRUE),
};

/* Compile-time defaults applied before the first shadow sync of this boot.
 * Not persisted across reboots (no NVS/settings backing yet), so every boot
 * re-applies (and logs) the full delta from these defaults to the platform's
 * current target. */
static struct app_shadow_config current_config = {
    .log = false,
    .telemetry_interval = 60,
    .reboot = false,
};

/* Sets every registered module's runtime filter level in one call, so the
 * shadow's "log" field can actually silence/restore logging rather than just
 * being logged and ignored. NULL backend applies to all backends+frontend. */
static void set_all_log_levels(uint32_t level) {
  uint32_t module_count = log_src_cnt_get(Z_LOG_LOCAL_DOMAIN_ID);

  for (uint32_t i = 0; i < module_count; i++) {
    log_filter_set(NULL, Z_LOG_LOCAL_DOMAIN_ID, (int16_t)i, level);
  }
}

/* Reports uptime via the device telemetry path (pigeon_set_shadow_param() +
 * pigeon_shadow_flush(), POSTing to <endpoint>/telemetry). Unrelated to
 * shadow config ack (see pigeon_shadow_report() below). */
static void report_uptime(void) {
  char uptime_s[16];

  snprintk(uptime_s, sizeof(uptime_s), "%lld", k_uptime_get() / 1000);

  int err = pigeon_set_shadow_param("uptime_s", uptime_s);

  if (!err) {
    err = pigeon_shadow_flush();
  }

  if (err) {
    LOG_WRN("Telemetry report failed: %d", err);
  }
}

int shadow_sync(void) {
  struct pigeon_shadow_doc doc;
  int err = pigeon_shadow_get(&doc);

  if (err) {
    LOG_ERR("Failed to fetch device shadow: %d", err);
    return err;
  }

  LOG_INF(
      "Shadow fetched: target_version=%d current_version=%d updated_at=%lld", doc.target_version,
      doc.current_version, doc.updated_at
  );

  report_uptime();

  if (doc.target_version == doc.current_version) {
    LOG_INF("Shadow already converged at version %d; nothing to apply", doc.current_version);
    return 0;
  }

  /* target_config is only valid until the next pigeon_shadow_get() call, and
   * json_obj_parse() modifies its input in place, so work on a local copy. */
  char config_buf[256];

  strncpy(config_buf, doc.target_config, sizeof(config_buf) - 1);
  config_buf[sizeof(config_buf) - 1] = '\0';

  /* Seed with the current values (reboot always defaults back to false: it's
   * a one-shot command, not a persistent field) so keys absent from
   * target_config (a partial update) retain their current value. */
  struct app_shadow_config target = current_config;
  target.reboot = false;

  int64_t decoded = json_obj_parse(
      config_buf, strlen(config_buf), app_shadow_config_descr, ARRAY_SIZE(app_shadow_config_descr),
      &target
  );

  if (decoded < 0) {
    LOG_ERR("Failed to parse shadow target_config '%s': %lld", doc.target_config, decoded);
    return (int)decoded;
  }

  if (target.log != current_config.log) {
    LOG_INF(
        "Shadow v%d: log %s -> %s", doc.target_version, current_config.log ? "true" : "false",
        target.log ? "true" : "false"
    );
    set_all_log_levels(target.log ? CONFIG_LOG_DEFAULT_LEVEL : LOG_LEVEL_NONE);
  }

  if (target.telemetry_interval != current_config.telemetry_interval) {
    LOG_INF(
        "Shadow v%d: telemetry_interval %d -> %d", doc.target_version,
        current_config.telemetry_interval, target.telemetry_interval
    );
  }

  current_config.log = target.log;
  current_config.telemetry_interval = target.telemetry_interval;

  LOG_INF(
      "Applied shadow v%d: log=%s telemetry_interval=%d", doc.target_version,
      current_config.log ? "true" : "false", current_config.telemetry_interval
  );

  /* Confirm what was actually applied back to the platform, same
   * report-before-reboot ordering as https_init's shadow.c: the shadow
   * must converge on the platform side before this device drops off the
   * network for the reboot below. */
  char report_buf[128];
  int encode_err = json_obj_encode_buf(
      app_shadow_config_descr, ARRAY_SIZE(app_shadow_config_descr), &current_config, report_buf,
      sizeof(report_buf)
  );

  if (encode_err) {
    LOG_ERR("Failed to encode current_config for shadow report: %d", encode_err);
  } else {
    int report_err = pigeon_shadow_report(doc.target_version, report_buf);

    if (report_err) {
      LOG_WRN("Shadow report-back failed: %d", report_err);
    } else {
      LOG_INF("Reported current_config back to platform at v%d", doc.target_version);
    }
  }

  /* Demonstrates command-via-shadow: "reboot" is a one-shot command rather
   * than a persistent field, so it's deliberately excluded from
   * current_config above -- otherwise it would never be seen as "changed"
   * again and the device would reboot on every single poll once set true.
   *
   * Disconnects WiFi gracefully first: unlike https_init's nRF91 modem,
   * ESP32-C6 has no reset-loop protection to trip, but leaving the AP
   * holding a stale association until it times out is still worth avoiding
   * with a clean conn_mgr teardown. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; disconnecting and rebooting now", doc.target_version);
#if defined(CONFIG_PIGEON_WS)
    /* Graceful CLOSE before the network goes down, same reasoning as the
     * WiFi teardown right below: let the server see a clean disconnect
     * instead of discovering the drop only via a dead ping/idle timeout. */
    pigeon_ws_stop();
#endif
    wifi_disconnect();
    sys_reboot(SYS_REBOOT_COLD);
  }

  return 0;
}

void shadow_loop(void) {
  while (1) {
    shadow_sync();

#if defined(CONFIG_PIGEON_WS)
    /* telemetry_interval remains the safety-net poll period while the WS
     * socket is down; a pushed shadow_update or a fresh CONNECTED event
     * (see shadow_ws_event_cb()) collapses the wait to ~instant instead. */
    LOG_INF("Next shadow poll in <=%d s (or sooner on WS push)", current_config.telemetry_interval);
    k_sem_take(&shadow_wakeup, K_SECONDS(current_config.telemetry_interval));
#else
    LOG_INF("Next shadow poll in %d s", current_config.telemetry_interval);
    k_sleep(K_SECONDS(current_config.telemetry_interval));
#endif
  }
}
