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

/* Signaled by shadow_event_cb() (called from pigeon's MQTT worker thread)
 * to wake shadow_loop() immediately instead of waiting out its
 * telemetry_interval. */
K_SEM_DEFINE(shadow_wakeup, 0, 1);

void shadow_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc *shadow) {
  ARG_UNUSED(shadow); /* A push is a wakeup here, not a data path:
                       * shadow_sync() reads the same retained value back
                       * through pigeon_shadow_get(), which costs nothing on
                       * this connector and keeps one code path for the
                       * pushed and the periodic case alike. */

  switch (ev) {
    case PIGEON_EVENT_CONNECTED:
      /* A fresh session seeds its retained target immediately, and the
       * platform may well have moved on while this device was away. */
    case PIGEON_EVENT_SHADOW_UPDATE:
      k_sem_give(&shadow_wakeup);
      break;
    case PIGEON_EVENT_DISCONNECTED:
      /* Informational: the reconnect is pigeon's own, and the periodic tick
       * stays as the safety net meanwhile. */
      break;
  }
}

/* pigeon_shadow_doc's target_config is an opaque JSON string as far as the
 * pigeon library is concerned (see pigeon.h); this app decides what the
 * fields inside it mean and how to apply them. */
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

/* Reports uptime and this boot's pass count as ordinary telemetry: one
 * pigeon_telemetry_set() per key, then ONE pigeon_telemetry_flush(), which
 * on this connector publishes a single message to pigeon/telemetry.
 * Whether that publish is acknowledged is a build-time choice
 * (CONFIG_PIGEON_MQTT_TELEMETRY_QOS1); either way this is a different path
 * from the shadow report below, which acks config rather than metrics.
 * poll_count restarting from 1 doubles as a cheap reboot indicator. */
static void report_telemetry(void) {
  static unsigned int poll_count;
  char buf[16];

  snprintk(buf, sizeof(buf), "%lld", (long long)(k_uptime_get() / 1000));

  int err = pigeon_telemetry_set("uptime_s", buf);

  snprintk(buf, sizeof(buf), "%u", ++poll_count);

  if (!err) {
    err = pigeon_telemetry_set("poll_count", buf);
  }

  if (!err) {
    err = pigeon_telemetry_flush();
  }

  if (err) {
    LOG_WRN("Telemetry report failed: %d", err);
  }
}

int shadow_sync(void) {
  struct pigeon_shadow_doc doc;
  /* Reads the retained target the broker pushed, rather than fetching
   * anything: on this connector the platform's config arrives on its own
   * and this call serves whatever landed last (see pigeon.h). */
  int err = pigeon_shadow_get(&doc);

  if (err) {
    LOG_ERR("No target shadow available: %d", err);
    return err;
  }

  LOG_INF(
      "Shadow: target_version=%d current_version=%d updated_at=%lld", doc.target_version,
      doc.current_version, doc.updated_at
  );

  report_telemetry();

  if (doc.target_version == doc.current_version) {
    LOG_INF("Shadow already converged at version %d; nothing to apply", doc.current_version);
    return 0;
  }

  /* target_config points into pigeon's own storage, which the next push
   * rewrites, and json_obj_parse() modifies its input in place -- so work
   * on a local copy. */
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
    LOG_ERR("Failed to parse shadow target_config: %lld", decoded);
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

  /* Confirm what was actually applied back to the platform: a QoS 1 publish
   * to pigeon/shadow/report, acknowledged only once the platform has taken
   * it. current_version is the target_version just applied, not re-derived
   * server-side, so this must be sent even if a newer target is already on
   * its way. */
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

  /* Command-via-shadow, the same convention the other samples demonstrate:
   * "reboot" is a one-shot rather than a persistent field, so it is
   * deliberately excluded from current_config above -- otherwise it would
   * never look "changed" again and the device would reboot on every pass
   * once set.
   *
   * The MQTT session is ended properly first (a real DISCONNECT rather than
   * a dropped socket, which the broker would read as ungraceful and answer
   * by publishing this session's will), then the link. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; disconnecting and rebooting now", doc.target_version);
    pigeon_mqtt_stop();
    wifi_disconnect();
    sys_reboot(SYS_REBOOT_COLD);
  }

  return 0;
}

void shadow_loop(void) {
  while (1) {
    shadow_sync();

    /* telemetry_interval is the periodic tick that keeps telemetry flowing;
     * a pushed target (or a fresh session) collapses the wait to ~instant,
     * which is the whole point of a retained shadow instead of a poll. */
    LOG_INF(
        "Next pass in <=%d s (or sooner on a pushed shadow)", current_config.telemetry_interval
    );
    k_sem_take(&shadow_wakeup, K_SECONDS(current_config.telemetry_interval));
  }
}
