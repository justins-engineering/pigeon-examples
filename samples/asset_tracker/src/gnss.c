/** @headerfile gnss.h
 *  The nRF91 modem's own GNSS receiver, read through nrf_modem_gnss.
 */
#include "gnss.h"

#include <modem/lte_lc.h>
#include <nrf_modem_gnss.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss, CONFIG_ASSET_TRACKER_LOG_LEVEL);

/* GPS has to be in the modem's system mode for the receiver to run at all;
 * without it every call below succeeds and no fix ever arrives. */
BUILD_ASSERT(
    IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS) ||
        IS_ENABLED(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS) ||
        IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS),
    "the modem's system mode must include GPS"
);

static struct k_mutex pvt_lock;
static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static bool have_pvt;

static void gnss_event_handler(int event) {
  /* No NMEA stream is subscribed and no assistance is requested, so a
   * position frame is the only event this sample asked for. */
  if (event != NRF_MODEM_GNSS_EVT_PVT) {
    return;
  }

  struct nrf_modem_gnss_pvt_data_frame pvt;

  if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT) != 0) {
    LOG_WRN("Failed to read the position frame");
    return;
  }

  k_mutex_lock(&pvt_lock, K_FOREVER);
  last_pvt = pvt;
  have_pvt = true;
  k_mutex_unlock(&pvt_lock);
}

int tracker_gnss_init(void) {
  k_mutex_init(&pvt_lock);

  /* Attaching to LTE activates LTE alone, and GNSS stays off: every
   * nrf_modem_gnss call then fails with -NRF_EACCES. This mode is additive
   * and leaves the LTE connection alone. */
  int err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);

  if (err) {
    LOG_ERR("Failed to activate the GNSS functional mode: %d", err);
    return err;
  }

  err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
  if (err) {
    LOG_ERR("Failed to set the GNSS event handler: %d", err);
    return err;
  }

  err = nrf_modem_gnss_fix_interval_set(CONFIG_ASSET_TRACKER_GNSS_FIX_INTERVAL_SEC);
  if (err) {
    LOG_ERR("Failed to set the GNSS fix interval: %d", err);
    return err;
  }

  err = nrf_modem_gnss_fix_retry_set(CONFIG_ASSET_TRACKER_GNSS_FIX_RETRY_SEC);
  if (err) {
    LOG_ERR("Failed to set the GNSS fix retry timeout: %d", err);
    return err;
  }

  err = nrf_modem_gnss_start();
  if (err) {
    LOG_ERR("Failed to start GNSS: %d", err);
    return err;
  }

  LOG_INF(
      "GNSS started: a fix every %d s, abandoned after %d s of searching",
      CONFIG_ASSET_TRACKER_GNSS_FIX_INTERVAL_SEC, CONFIG_ASSET_TRACKER_GNSS_FIX_RETRY_SEC
  );

  return 0;
}

void tracker_gnss_get_latest(struct tracker_position* out) {
  memset(out, 0, sizeof(*out));

  k_mutex_lock(&pvt_lock, K_FOREVER);

  if (!have_pvt) {
    k_mutex_unlock(&pvt_lock);
    LOG_INF("No position frame received yet");
    return;
  }

  struct nrf_modem_gnss_pvt_data_frame pvt = last_pvt;

  k_mutex_unlock(&pvt_lock);

  /* Counted whether or not each satellite was used in the fix, so "nothing
   * in view" and "six in view, still no fix" stay distinguishable. */
  int sats = 0;

  for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
    if (pvt.sv[i].sv != 0) {
      sats++;
    }
  }

  out->sats = sats;

  if ((pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) == 0) {
    out->fix_quality = TRACKER_FIX_NONE;
    return;
  }

  out->fix_quality = TRACKER_FIX_REAL;
  out->latitude = pvt.latitude;
  out->longitude = pvt.longitude;
  out->altitude_m = pvt.altitude;
  out->speed_mps = pvt.speed;
  out->heading_deg = pvt.heading;
}
