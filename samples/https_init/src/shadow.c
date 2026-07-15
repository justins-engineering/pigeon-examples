/** @headerfile shadow.h */
#include "shadow.h"

#include <pigeon.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(shadow);

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

  /* Demonstrates command-via-shadow (see pigeon's CLAUDE.md: pidgeiot has no
   * formal generation-counter/command-ack model yet, only this raw
   * target_config JSON): "reboot" is a one-shot command rather than a
   * persistent field, so it's deliberately excluded from current_config
   * above -- otherwise it would never be seen as "changed" again and the
   * device would reboot on every single poll once set true. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; rebooting now", doc.target_version);
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
