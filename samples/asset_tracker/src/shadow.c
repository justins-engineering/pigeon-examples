/** @headerfile shadow.h */
#include "shadow.h"

#include <pigeon.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>

#include "gnss.h"
#include "net_connect.h"

LOG_MODULE_REGISTER(shadow);

/* target_config is opaque JSON to the library; this application decides what
 * its keys mean. */
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

/* Defaults applied before the first sync of a boot. Nothing persists across
 * reboots, so every boot re-applies the platform's target from here. */
static struct app_shadow_config current_config = {
    .log = false,
    .telemetry_interval = 60,
    .reboot = false,
};

/* One call covers every module, so the shadow's "log" field really silences
 * and restores output. */
static void set_all_log_levels(uint32_t level) {
  uint32_t module_count = log_src_cnt_get(Z_LOG_LOCAL_DOMAIN_ID);

  for (uint32_t i = 0; i < module_count; i++) {
    log_filter_set(NULL, Z_LOG_LOCAL_DOMAIN_ID, (int16_t)i, level);
  }
}

static void queue_metric(const char* key, const char* value) {
  int err = pigeon_telemetry_set(key, value);

  if (err) {
    LOG_WRN("Failed to queue telemetry '%s': %d", key, err);
  }
}

/* Fix quality and satellite count are reported even with no fix, so a
 * dashboard can tell a device searching in a basement from a silent one. The
 * position itself is only meaningful once there is a fix. */
static void queue_position(void) {
  struct tracker_position pos;

  tracker_gnss_get_latest(&pos);

  char buf[32];

  snprintk(buf, sizeof(buf), "%d", (int)pos.fix_quality);
  queue_metric("gps_fix_quality", buf);

  snprintk(buf, sizeof(buf), "%d", pos.sats);
  queue_metric("gps_sats", buf);

  if (pos.fix_quality == TRACKER_FIX_NONE) {
    LOG_INF("No fix yet, %d satellites tracked; position not reported", pos.sats);
    return;
  }

  snprintk(buf, sizeof(buf), "%.6f", pos.latitude);
  queue_metric("gps_lat", buf);

  snprintk(buf, sizeof(buf), "%.6f", pos.longitude);
  queue_metric("gps_lon", buf);

  snprintk(buf, sizeof(buf), "%.1f", (double)pos.altitude_m);
  queue_metric("gps_alt_m", buf);

  snprintk(buf, sizeof(buf), "%.2f", (double)pos.speed_mps);
  queue_metric("gps_speed_mps", buf);

  snprintk(buf, sizeof(buf), "%.1f", (double)pos.heading_deg);
  queue_metric("gps_heading_deg", buf);

  LOG_INF(
      "Position (%s): %.6f,%.6f alt=%.1fm speed=%.2fm/s heading=%.1fdeg sats=%d",
      pos.fix_quality == TRACKER_FIX_SIMULATED ? "simulated" : "fix", pos.latitude, pos.longitude,
      (double)pos.altitude_m, (double)pos.speed_mps, (double)pos.heading_deg, pos.sats
  );
}

/* Every key of a cycle rides one report, which over LTE-M is one TLS
 * handshake instead of eight. */
static void report_telemetry(void) {
  char buf[21];

  snprintk(buf, sizeof(buf), "%lld", (long long)(k_uptime_get() / 1000));
  queue_metric("uptime_s", buf);

  queue_position();

  int err = pigeon_telemetry_flush();

  if (err) {
    LOG_WRN("Telemetry report failed: %d; the queued keys wait for the next poll", err);
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

  report_telemetry();

  if (doc.target_version == doc.current_version) {
    LOG_INF("Shadow already converged at version %d; nothing to apply", doc.current_version);
    return 0;
  }

  /* json_obj_parse() edits its input, and target_config only lives until the
   * next fetch. */
  char config_buf[CONFIG_PIGEON_SHADOW_CONFIG_MAX];

  strncpy(config_buf, doc.target_config, sizeof(config_buf) - 1);
  config_buf[sizeof(config_buf) - 1] = '\0';

  /* Keys absent from a partial update keep their current value. reboot is a
   * one-shot command and always starts false. */
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

  /* "reboot" is a command, not state: kept out of current_config so it is
   * never seen as unchanged and re-run on every poll. net_disconnect() comes
   * first because a modem reset without a graceful power-off trips its
   * reset-loop protection. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; disconnecting and rebooting now", doc.target_version);
    net_disconnect();
    pigeon_reboot();
  }

  return 0;
}

void shadow_loop(void) {
  while (1) {
    shadow_sync();

    LOG_INF("Next shadow poll in %d s", current_config.telemetry_interval);
    k_sleep(K_SECONDS(current_config.telemetry_interval));
  }
}
