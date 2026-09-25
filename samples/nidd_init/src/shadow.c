/** @headerfile shadow.h */
#include "shadow.h"

#include <modem/lte_lc.h>
#include <pigeon.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(shadow);

/* The carrier asks for at most four radio accesses an hour, uplink and downlink together, so a
 * wake comes no more often than every 15 minutes. */
#define WAKE_MIN_SEC 900
/* Under CONFIG_PIGEON_TELEMETRY_BATCH_DEPTH, so no reading is dropped to make room. */
#define READINGS_PER_WAKE 4

/* Signalled from the library's receive thread, so a pushed shadow is applied and reported while
 * the connection it arrived on is still up. */
K_SEM_DEFINE(shadow_wakeup, 0, 1);

void shadow_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc* shadow) {
  ARG_UNUSED(shadow);

  /* A report waits for a reply that this callback's thread receives, so it is made from
   * shadow_loop()'s thread instead. */
  if (ev == PIGEON_EVENT_SHADOW_UPDATE) {
    k_sem_give(&shadow_wakeup);
  }
}

/* target_config is opaque JSON to the library; this application decides what its keys mean. */
struct app_config {
  int telemetry_interval;
  bool reboot;
};

static const struct json_obj_descr app_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct app_config, telemetry_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct app_config, reboot, JSON_TOK_TRUE),
};

/* Nothing persists across reboots: every boot applies the platform's target from here. */
static struct app_config applied = {.telemetry_interval = 1200};
static int32_t applied_version = -1;
/* The applied version's config asked for a reboot. */
static bool reboot_asked;

/* Buffered readings would be lost with the RAM holding them, and the modem is powered off
 * first so the next boot can attach at once. */
static void sample_reboot(void) {
  LOG_WRN("Shadow v%d requested a reboot", applied_version);
  (void)pigeon_telemetry_flush_now();
  (void)pigeon_nidd_stop();
  (void)lte_lc_power_off();
  pigeon_reboot();
}

static int apply(const struct pigeon_shadow_doc* doc) {
  /* json_obj_parse() edits its input. */
  char buf[CONFIG_PIGEON_SHADOW_CONFIG_MAX];
  /* Keys absent from the target keep their value. reboot is a one-shot command. */
  struct app_config target = applied;

  strncpy(buf, doc->target_config, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  target.reboot = false;

  int64_t decoded =
      json_obj_parse(buf, strlen(buf), app_config_descr, ARRAY_SIZE(app_config_descr), &target);

  if (decoded < 0) {
    LOG_ERR("Shadow v%d: target_config did not parse: %lld", doc->target_version, decoded);
    return (int)decoded;
  }

  if (target.telemetry_interval < WAKE_MIN_SEC) {
    LOG_WRN(
        "Shadow v%d: telemetry_interval %d is under the carrier's 15-minute floor, using %d",
        doc->target_version, target.telemetry_interval, WAKE_MIN_SEC
    );
    target.telemetry_interval = WAKE_MIN_SEC;
  }

  applied = target;
  applied_version = doc->target_version;
  reboot_asked = target.reboot;
  LOG_INF("Applied shadow v%d: telemetry_interval=%d", applied_version, applied.telemetry_interval);

  return 0;
}

int shadow_sync(void) {
  struct pigeon_shadow_doc doc;
  int err = pigeon_shadow_get(&doc);

  if (err) {
    LOG_WRN("No shadow yet: %d", err);
    return err;
  }

  if (doc.target_version > applied_version) {
    err = apply(&doc);
    if (err) {
      return err;
    }
  }

  if (applied_version <= doc.current_version) {
    return 0;
  }

  char report[48];

  snprintk(report, sizeof(report), "{\"telemetry_interval\":%d}", applied.telemetry_interval);
  err = pigeon_shadow_report(applied_version, report);

  if (err == -ETIMEDOUT) {
    LOG_WRN("Shadow v%d report unconfirmed: reporting again at the next wake", applied_version);
    return 0;
  }
  if (err) {
    LOG_WRN("Shadow v%d report failed: %d", applied_version, err);
    return 0;
  }

  LOG_INF("Reported shadow v%d", applied_version);

  /* Only after the platform confirmed this version: the next boot then finds it converged and
   * does not reboot again, and an unconfirmed report cannot turn into a reboot loop. */
  if (reboot_asked) {
    sample_reboot();
  }

  return 0;
}

static void take_reading(void) {
  /* Counts from 1 at every boot, so a gap in the series shows which reading was lost. */
  static unsigned int reading;
  char buf[21];

  snprintk(buf, sizeof(buf), "%lld", (long long)(k_uptime_get() / MSEC_PER_SEC));

  int err = pigeon_telemetry_set("uptime_s", buf);

  snprintk(buf, sizeof(buf), "%u", ++reading);
  if (!err) {
    err = pigeon_telemetry_set("reading", buf);
  }
  if (!err) {
    err = pigeon_telemetry_record();
  }
  if (err) {
    LOG_WRN("Reading %u not recorded: %d", reading, err);
  }
}

void shadow_loop(void) {
  int64_t next_ms = k_uptime_get();
  int taken = 0;

  /* The reply to HELLO carries the target, so this first pass costs no access of its own. */
  (void)shadow_sync();

  while (1) {
    int64_t now = k_uptime_get();

    if (now < next_ms) {
      if (k_sem_take(&shadow_wakeup, K_MSEC(next_ms - now)) == 0) {
        (void)shadow_sync();
      }
      continue;
    }

    take_reading();
    next_ms += (int64_t)applied.telemetry_interval * MSEC_PER_SEC / READINGS_PER_WAKE;

    if (++taken < READINGS_PER_WAKE) {
      continue;
    }

    /* The wake: one frame carries the four readings, and anything the platform owes this
     * device comes back on the connection that frame opened. */
    taken = 0;

    int err = pigeon_telemetry_flush_now();

    if (err) {
      LOG_WRN("Telemetry not sent: %d (the readings stay buffered)", err);
    }
    (void)shadow_sync();
  }
}
