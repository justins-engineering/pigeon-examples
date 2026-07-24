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

#include "gnss.h"
#include "net/connection_manager.h"

LOG_MODULE_REGISTER(shadow);

/* pigeon_shadow_doc's target_config is an opaque JSON string as far as the
 * pigeon library is concerned (see pigeon.h); this app decides what the
 * fields inside it mean and how to apply them -- same
 * log/telemetry_interval/reboot convention as https_init's shadow.c (no
 * "firmware" key here -- this sample doesn't do FOTA, see its README). */
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
 * current target -- same convention as https_init's shadow.c. */
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

/* Single-key set+flush pair (pigeon_set_shadow_param()/pigeon_shadow_flush()
 * queue and send exactly one shadow delta at a time -- see pigeon's
 * CLAUDE.md), shared by every telemetry key this sample reports so
 * report_uptime()/report_position() below don't each repeat the same
 * queue-then-flush-then-log-on-failure boilerplate. POSTs to
 * <endpoint>/telemetry, not the shadow config-ack endpoint -- see
 * pigeon_shadow_flush()'s own doc comment in pigeon.h. */
static void report_metric(const char *key, const char *val) {
  int err = pigeon_set_shadow_param(key, val);

  if (!err) {
    err = pigeon_shadow_flush();
  }

  if (err) {
    LOG_WRN("Telemetry report failed for '%s'='%s': %d", key, val, err);
  }
}

static void report_uptime(void) {
  char uptime_s[16];

  snprintk(uptime_s, sizeof(uptime_s), "%lld", k_uptime_get() / 1000);
  report_metric("uptime_s", uptime_s);
}

/* Reports the latest GNSS (or simulated) position as a handful of numeric
 * telemetry keys -- gps_sats/gps_fix_quality are always reported, even with
 * no fix at all, so a dashboard can distinguish "searching, 0 sats" from
 * "searching, 6 sats tracked" from "converged" rather than just seeing
 * nothing. The position fields themselves (lat/lon/alt/speed/heading) are
 * only meaningful -- and only reported -- once fix_quality says there's an
 * actual fix (real or simulated) to report. */
static void report_position(void) {
  struct tracker_position pos;

  tracker_gnss_get_latest(&pos);

  char buf[32];

  snprintk(buf, sizeof(buf), "%d", (int)pos.fix_quality);
  report_metric("gps_fix_quality", buf);

  snprintk(buf, sizeof(buf), "%d", pos.sats);
  report_metric("gps_sats", buf);

  if (pos.fix_quality == TRACKER_FIX_NONE) {
    LOG_INF("No GNSS fix yet (%d satellites tracked); position not reported", pos.sats);
    return;
  }

  snprintk(buf, sizeof(buf), "%.6f", pos.latitude);
  report_metric("gps_lat", buf);

  snprintk(buf, sizeof(buf), "%.6f", pos.longitude);
  report_metric("gps_lon", buf);

  snprintk(buf, sizeof(buf), "%.1f", (double)pos.altitude_m);
  report_metric("gps_alt_m", buf);

  snprintk(buf, sizeof(buf), "%.2f", (double)pos.speed_mps);
  report_metric("gps_speed_mps", buf);

  snprintk(buf, sizeof(buf), "%.1f", (double)pos.heading_deg);
  report_metric("gps_heading_deg", buf);

  LOG_INF(
      "Position (%s): %.6f,%.6f alt=%.1fm speed=%.2fm/s heading=%.1fdeg sats=%d",
      pos.fix_quality == TRACKER_FIX_SIMULATED ? "SIMULATED" : "fix", pos.latitude, pos.longitude,
      (double)pos.altitude_m, (double)pos.speed_mps, (double)pos.heading_deg, pos.sats
  );
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
  report_position();

  if (doc.target_version == doc.current_version) {
    LOG_INF("Shadow already converged at version %d; nothing to apply", doc.current_version);
    return 0;
  }

  /* target_config is only valid until the next pigeon_shadow_get() call, and
   * json_obj_parse() modifies its input in place, so work on a local copy. */
  char config_buf[320];

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

  /* Confirm what was actually applied back to the platform -- see
   * pigeon_shadow_report()'s doc comment in pigeon.h and https_init's
   * shadow.c for why this is a separate call from the telemetry reports
   * above. */
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

  /* "reboot" is a one-shot command, deliberately excluded from
   * current_config above so it isn't seen as "changed" (and re-fire) on
   * every subsequent poll. Gracefully powers the modem off first
   * (lte_disconnect() -> lte_lc_power_off()) rather than calling
   * sys_reboot() directly -- an ungraceful reset trips the nRF91 modem's
   * reset-loop protection and refuses LTE attach for 30 minutes, see this
   * repo's CLAUDE.md "Modem reset safety" and connection_manager.c. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; disconnecting and rebooting now", doc.target_version);
    lte_disconnect();
    sys_reboot(SYS_REBOOT_COLD);
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
