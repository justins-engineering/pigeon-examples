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

/* Same struct/decode pattern as https_init's shadow.c. target_config is
 * opaque JSON to the pigeon library, so what a key means is this app's
 * decision, and "firmware" is no exception: struct pigeon_fota_info
 * (pigeon.h) is just the decode target for that one key, the same way this
 * struct is for log/telemetry_interval/reboot. Only the download and flash
 * mechanics live in the library, because those need its HTTPS transport
 * internals. The key is decoded only on a build that can act on it. */
struct app_shadow_config {
  bool log;
  int telemetry_interval;
  bool reboot;
#if defined(CONFIG_PIGEON_FOTA)
  struct pigeon_fota_info firmware;
#endif
};

#if defined(CONFIG_PIGEON_FOTA)
static const struct json_obj_descr app_firmware_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct pigeon_fota_info, version, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct pigeon_fota_info, size, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct pigeon_fota_info, sha256, JSON_TOK_STRING_BUF),
};
#endif

static const struct json_obj_descr app_shadow_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct app_shadow_config, log, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM(struct app_shadow_config, telemetry_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct app_shadow_config, reboot, JSON_TOK_TRUE),
#if defined(CONFIG_PIGEON_FOTA)
    JSON_OBJ_DESCR_OBJECT(struct app_shadow_config, firmware, app_firmware_descr),
#endif
};

/* Compile-time defaults applied before the first shadow sync of this boot.
 * Not persisted across reboots (no NVS/settings backing yet), so every boot
 * re-applies (and logs) the full delta from these defaults to the platform's
 * current target. firmware.version defaults to this build's own compiled-in
 * version rather than blank, so every report names the image actually
 * running even on a boot where no update happened, and a boot into a freshly
 * applied image starts back at "target == running" with no extra
 * bookkeeping. */
static struct app_shadow_config current_config = {
    .log = false,
    .telemetry_interval = 60,
    .reboot = false,
#if defined(CONFIG_PIGEON_FOTA)
    .firmware = {.version = CONFIG_PIGEON_FOTA_CURRENT_VERSION, .size = 0, .sha256 = ""},
#endif
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

/* Reports uptime and this boot's poll count via the device telemetry path:
 * pigeon_telemetry_set() per key, then ONE pigeon_telemetry_flush() -- both
 * keys ride a single report to <endpoint>/telemetry (one HTTPS POST,
 * dovecote's report_telemetry_device upserting every key in the body).
 * Unrelated to shadow config ack (see pigeon_shadow_report() below).
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
  int err = pigeon_shadow_get(&doc);

  if (err) {
    LOG_ERR("Failed to fetch device shadow: %d", err);
    return err;
  }

  LOG_INF(
      "Shadow fetched: target_version=%d current_version=%d updated_at=%lld", doc.target_version,
      doc.current_version, doc.updated_at
  );

#if defined(CONFIG_PIGEON_FOTA)
  /* A successful shadow fetch (network, auth and JSON parse all worked) is
   * this app's definition of a healthy boot. Run it on every sync rather
   * than only the first: boot_is_img_confirmed() makes it a no-op once
   * confirmed, and placing it before the convergence early-return below
   * means a boot that lands already converged, the normal case right after
   * a swap, still confirms instead of waiting for MCUboot to revert it. */
  pigeon_fota_confirm_boot();
#endif

  report_telemetry();

  if (doc.target_version == doc.current_version) {
    LOG_INF("Shadow already converged at version %d; nothing to apply", doc.current_version);
    return 0;
  }

  /* target_config is only valid until the next pigeon_shadow_get() call, and
   * json_obj_parse() modifies its input in place, so work on a local copy. */
#if defined(CONFIG_PIGEON_FOTA)
  /* A "firmware" object is by far the largest key this sample decodes, so
   * the copy tracks the library's own cap on one shadow config rather than
   * a size chosen for the three scalar keys alone. */
  char config_buf[CONFIG_PIGEON_SHADOW_CONFIG_MAX];
#else
  char config_buf[256];
#endif

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

#if defined(CONFIG_PIGEON_FOTA)
  /* pigeon_fota_update_available() compares the offered version against
   * this build's own compiled-in one, not against current_config, so a
   * target_config carrying no "firmware" key at all is correctly a no-op:
   * target.firmware was seeded from current_config above and so already
   * names the running image. */
  bool firmware_applied = false;
  /* Set when the shadow named a firmware image this poll did not end up
   * running. Convergence is the platform's signal that everything the
   * target asked for is in place, and it is also what makes the next poll
   * return early, so reporting it here would leave a dashboard showing a
   * converged pigeon that is still on the old image and a device that has
   * stopped trying to change that. */
  bool firmware_unconverged = false;

  if (!pigeon_fota_update_available(&target.firmware)) {
    /* Running what is offered, so drop any budget still held against this
     * target and let a later re-offer of the same version start full. */
    pigeon_fota_attempts_clear();
  } else if (!pigeon_fota_attempt_allowed(&target.firmware, doc.target_version)) {
    LOG_ERR(
        "FOTA: attempt budget spent for firmware %s at shadow v%d; not downloading again until "
        "the shadow is written anew",
        target.firmware.version, doc.target_version
    );
    target.firmware = current_config.firmware;
    firmware_unconverged = true;
  } else {
    LOG_WRN(
        "Shadow v%d requests firmware %s (currently running %s); starting FOTA download",
        doc.target_version, target.firmware.version, CONFIG_PIGEON_FOTA_CURRENT_VERSION
    );

    int fota_err = pigeon_fota_apply(&target.firmware);

    if (fota_err) {
      LOG_ERR(
          "FOTA apply failed: %d; leaving current image running, will retry next poll", fota_err
      );
      /* Report the version still actually running rather than the one that
       * was asked for, so the report below cannot claim an image the device
       * never booted. */
      target.firmware = current_config.firmware;
      firmware_unconverged = true;
    } else {
      LOG_WRN("FOTA: image staged; will reboot after reporting shadow convergence");
      current_config.firmware = target.firmware;
      firmware_applied = true;
    }
  }
#endif

  /* Confirm what was actually applied back to the platform, same
   * report-before-reboot ordering as https_init's shadow.c: the shadow
   * must converge on the platform side before this device drops off the
   * network for the reboot below. */
#if defined(CONFIG_PIGEON_FOTA)
  char report_buf[256];
#else
  char report_buf[128];
#endif
  int encode_err = json_obj_encode_buf(
      app_shadow_config_descr, ARRAY_SIZE(app_shadow_config_descr), &current_config, report_buf,
      sizeof(report_buf)
  );
  /* The report still goes out either way: current_config is what this
   * device is genuinely running, including whatever else in this target it
   * did apply, and the platform only learns the firmware version actually
   * booted from here. Only the version it is reported AT changes, which is
   * what leaves the shadow short of its target. */
  int32_t report_version = doc.target_version;

#if defined(CONFIG_PIGEON_FOTA)
  if (firmware_unconverged) {
    report_version = doc.current_version;
    LOG_WRN(
        "Shadow v%d left unconverged: still running firmware %s, reporting at v%d so the next "
        "poll retries",
        doc.target_version, current_config.firmware.version, report_version
    );
  }
#endif

  if (encode_err) {
    LOG_ERR("Failed to encode current_config for shadow report: %d", encode_err);
  } else {
    int report_err = pigeon_shadow_report(report_version, report_buf);

    if (report_err) {
      LOG_WRN("Shadow report-back failed: %d", report_err);
    } else {
      LOG_INF("Reported current_config back to platform at v%d", report_version);
    }
  }

#if defined(CONFIG_PIGEON_FOTA)
  if (target.reboot && firmware_unconverged) {
    /* Convergence is this one-shot command's only record that it was already
     * carried out, and the report above deliberately withheld it, so obeying
     * the request now would reboot the device again on every poll for as
     * long as the firmware target keeps failing. */
    LOG_WRN(
        "Shadow v%d requested reboot; deferring it until the firmware target resolves",
        doc.target_version
    );
    target.reboot = false;
  }
#endif

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
    wifi_disconnect();
    sys_reboot(SYS_REBOOT_COLD);
  }

#if defined(CONFIG_PIGEON_FOTA)
  if (firmware_applied) {
    LOG_WRN(
        "FOTA: disconnecting and rebooting into newly staged firmware %s",
        current_config.firmware.version
    );
    wifi_disconnect();
    sys_reboot(SYS_REBOOT_COLD);
  }
#endif

  return 0;
}

void shadow_loop(void) {
  while (1) {
    shadow_sync();

    LOG_INF("Next shadow poll in %d s", current_config.telemetry_interval);
    k_sleep(K_SECONDS(current_config.telemetry_interval));
  }
}
