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

/* target_config is opaque JSON to the library; this application decides what
 * its keys mean. "firmware" decodes into the struct pigeon_fota_apply() takes. */
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

/* Defaults applied before the first sync of a boot. Nothing persists across
 * reboots, so every boot re-applies the platform's target from here.
 * firmware.version starts at this build's own version, which is what lets a
 * freshly booted image report itself as converged with no bookkeeping. */
static struct app_shadow_config current_config = {
    .log = false,
    .telemetry_interval = 60,
    .reboot = false,
#if defined(CONFIG_PIGEON_FOTA)
    .firmware = {.version = CONFIG_PIGEON_FOTA_CURRENT_VERSION, .size = 0, .sha256 = ""},
#endif
};

/* One call covers every module, so the shadow's "log" field really silences
 * and restores output. */
static void set_all_log_levels(uint32_t level) {
  uint32_t module_count = log_src_cnt_get(Z_LOG_LOCAL_DOMAIN_ID);

  for (uint32_t i = 0; i < module_count; i++) {
    log_filter_set(NULL, Z_LOG_LOCAL_DOMAIN_ID, (int16_t)i, level);
  }
}

/* Both keys ride one telemetry POST. poll_count restarting at 1 doubles as a
 * reboot indicator on the dashboard. */
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

#if defined(CONFIG_PIGEON_FOTA)
  /* A successful fetch is this application's definition of a healthy boot.
   * Runs before the convergence check so a new image that lands already
   * converged is still confirmed; a no-op once confirmed. */
  pigeon_fota_confirm_boot();
#endif

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

#if defined(CONFIG_PIGEON_FOTA)
  /* The library compares against this build's compiled-in version, not
   * current_config, so a "firmware" key absent from the update is a no-op. */
  bool firmware_applied = false;
  /* Set when the shadow named an image this poll did not end up running.
   * Convergence is what makes the next poll return early, so reporting it
   * would leave the platform showing converged while the device stays on the
   * old image and stops trying. */
  bool firmware_unconverged = false;

  if (!pigeon_fota_update_available(&target.firmware)) {
    /* Running what is offered, so release any budget still held against it and
     * let a later re-offer of the same version start full. */
    pigeon_fota_attempts_clear();
  } else if (!pigeon_fota_attempt_allowed(&target.firmware, doc.target_version)) {
    LOG_ERR(
        "FOTA: attempt budget spent for firmware %s at shadow v%d; not downloading it again "
        "until the shadow is written anew",
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
      /* The report below has to name the version still running. */
      target.firmware = current_config.firmware;
      firmware_unconverged = true;
    } else {
      LOG_WRN("FOTA: image staged; rebooting to let the new image report for itself");
      /* A staged image is not a booted one: the bootloader can still refuse
       * it, and the image that boots reports its own version on its first
       * poll. The cost is one poll cycle before the platform sees convergence. */
      firmware_unconverged = true;
      firmware_applied = true;
    }
  }
#endif

  /* Sized for the firmware object too, whose sha256 alone is 64 characters. */
  char report_buf[256];
  int encode_err = json_obj_encode_buf(
      app_shadow_config_descr, ARRAY_SIZE(app_shadow_config_descr), &current_config, report_buf,
      sizeof(report_buf)
  );
  /* The report always carries what is running. Only the version it is
   * reported at is held back, which is what leaves the shadow short of its
   * target. */
  int32_t report_version = doc.target_version;

#if defined(CONFIG_PIGEON_FOTA)
  if (firmware_unconverged) {
    report_version = doc.current_version;
    if (firmware_applied) {
      LOG_WRN(
          "Shadow v%d left unconverged: reporting at v%d as firmware %s, the version still "
          "running; the image being booted into reports its own on its first poll",
          doc.target_version, report_version, current_config.firmware.version
      );
    } else {
      LOG_WRN(
          "Shadow v%d left unconverged: still running firmware %s, reporting at v%d so the next "
          "poll retries",
          doc.target_version, current_config.firmware.version, report_version
      );
    }
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
    /* Convergence is the only record that this one-shot command ran, and the
     * report above withheld it, so obeying now would reboot on every poll
     * until the firmware target resolves. A shadow carrying both reboots
     * twice: into the new image, then once more for the command. */
    LOG_WRN(
        "Shadow v%d requested reboot; deferring it until the firmware target resolves",
        doc.target_version
    );
    target.reboot = false;
  }
#endif

  /* "reboot" is a command, not state: kept out of current_config so it is
   * never seen as unchanged and re-run on every poll. net_disconnect() comes
   * first because a modem reset without a graceful power-off trips its
   * reset-loop protection. */
  if (target.reboot) {
    LOG_WRN("Shadow v%d requested reboot; disconnecting and rebooting now", doc.target_version);
    net_disconnect();
    pigeon_reboot();
  }

#if defined(CONFIG_PIGEON_FOTA)
  if (firmware_applied) {
    LOG_WRN(
        "FOTA: disconnecting and rebooting into newly staged firmware %s", target.firmware.version
    );
    net_disconnect();
    pigeon_reboot();
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
