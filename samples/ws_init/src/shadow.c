/** @headerfile shadow.h */
#include "shadow.h"

#include <pigeon.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>

#include "net_connect.h"

LOG_MODULE_REGISTER(shadow);

/* Given by the WebSocket worker thread, taken by shadow_loop(). */
static K_SEM_DEFINE(shadow_wakeup, 0, 1);

void shadow_ws_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc* shadow) {
  /* The pushed document only lives for this callback, so the loop re-fetches
   * over HTTPS rather than copying it out. A push is a wakeup, not a payload. */
  ARG_UNUSED(shadow);

  switch (ev) {
    case PIGEON_EVENT_CONNECTED:
      /* The server sends no snapshot on accept, so a fresh socket may mean
       * the platform moved on while this device was away. */
    case PIGEON_EVENT_SHADOW_UPDATE:
      k_sem_give(&shadow_wakeup);
      break;
    case PIGEON_EVENT_DISCONNECTED:
      /* The poll interval is the safety net while the socket is down. */
      break;
  }
}

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

/* Both keys ride one telemetry report, sent as a WebSocket frame while the
 * socket is up and an HTTPS POST otherwise. poll_count restarting at 1
 * doubles as a reboot indicator on the dashboard. */
static void report_telemetry(void) {
  static unsigned int poll_count;
  char buf[21];

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
   * never seen as unchanged and re-run on every poll. The CLOSE frame and the
   * network teardown both come first, so the server sees a clean disconnect
   * instead of waiting out a dead socket, and a modem is powered off rather
   * than reset. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; disconnecting and rebooting now", doc.target_version);
    pigeon_ws_stop();
    net_disconnect();
    pigeon_reboot();
  }

  return 0;
}

void shadow_loop(void) {
  while (1) {
    shadow_sync();

    /* The interval is a ceiling, not a cadence: a pushed shadow or a fresh
     * connection collapses the wait to the round trip that carried it. */
    LOG_INF("Next shadow poll in <=%d s, sooner on a push", current_config.telemetry_interval);
    k_sem_take(&shadow_wakeup, K_SECONDS(current_config.telemetry_interval));
  }
}
