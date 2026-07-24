/** @headerfile gnss.h */
#include "gnss.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss, CONFIG_ASSET_TRACKER_LOG_LEVEL);

#if !defined(CONFIG_ASSET_TRACKER_SIM_GPS)

/* Real GNSS: the nRF91's built-in GNSS receiver, via nrf_modem's GNSS API. */

#include <math.h>
#include <nrf_modem_gnss.h>

/* GNSS needs GPS in the modem's own system mode bitmask to interleave
 * search/tracking windows with LTE reception automatically -- see
 * CONFIG_LTE_NETWORK_MODE_LTE_M_GPS in prj.conf and this file's header
 * comment. Same assertion nRF's own "Cellular: GNSS" sample makes, so a
 * misconfigured build fails loudly at compile time rather than silently
 * never producing a fix. */
BUILD_ASSERT(
    IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS) ||
        IS_ENABLED(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS) ||
        IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS),
    "CONFIG_LTE_NETWORK_MODE_LTE_M_GPS, CONFIG_LTE_NETWORK_MODE_NBIOT_GPS, or "
    "CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS must be enabled for GNSS to run"
);

static struct k_mutex pvt_lock;
static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static bool have_pvt;

static void gnss_event_handler(int event) {
  if (event != NRF_MODEM_GNSS_EVT_PVT) {
    /* NMEA/A-GNSS-request/etc events are all irrelevant here -- nmea_mask
     * defaults to 0 (no NMEA subscribed) and assistance is deliberately
     * not used (see this sample's README), so PVT is the only event this
     * app expects. */
    return;
  }

  struct nrf_modem_gnss_pvt_data_frame pvt;

  if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT) != 0) {
    LOG_WRN("Failed to read PVT data after EVT_PVT");
    return;
  }

  k_mutex_lock(&pvt_lock, K_FOREVER);
  last_pvt = pvt;
  have_pvt = true;
  k_mutex_unlock(&pvt_lock);
}

int tracker_gnss_init(void) {
  k_mutex_init(&pvt_lock);

  int err = nrf_modem_gnss_event_handler_set(gnss_event_handler);

  if (err) {
    LOG_ERR("Failed to set GNSS event handler: %d", err);
    return err;
  }

  err = nrf_modem_gnss_fix_interval_set(CONFIG_ASSET_TRACKER_GNSS_FIX_INTERVAL_SEC);
  if (err) {
    LOG_ERR("Failed to set GNSS fix interval: %d", err);
    return err;
  }

  err = nrf_modem_gnss_fix_retry_set(CONFIG_ASSET_TRACKER_GNSS_FIX_RETRY_SEC);
  if (err) {
    LOG_ERR("Failed to set GNSS fix retry: %d", err);
    return err;
  }

  err = nrf_modem_gnss_start();
  if (err) {
    LOG_ERR("Failed to start GNSS: %d", err);
    return err;
  }

  LOG_INF(
      "GNSS started: periodic fixes every %d s (retry timeout %d s)",
      CONFIG_ASSET_TRACKER_GNSS_FIX_INTERVAL_SEC, CONFIG_ASSET_TRACKER_GNSS_FIX_RETRY_SEC
  );

  return 0;
}

void tracker_gnss_get_latest(struct tracker_position *out) {
  memset(out, 0, sizeof(*out));

  k_mutex_lock(&pvt_lock, K_FOREVER);

  if (!have_pvt) {
    k_mutex_unlock(&pvt_lock);
    LOG_INF("No PVT notification received yet");
    return;
  }

  struct nrf_modem_gnss_pvt_data_frame pvt = last_pvt;

  k_mutex_unlock(&pvt_lock);

  /* Satellites currently tracked, whether or not each ended up used in the
   * fix -- reported even when there's no fix at all, so "0 sats tracked"
   * vs. "6 sats tracked, still no fix" are distinguishable indoor/outdoor
   * signals rather than collapsing to the same "no data". */
  int sats = 0;

  for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
    if (pvt.sv[i].sv != 0) {
      sats++;
    }
  }

  out->sats = sats;

  bool fix_valid = (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) != 0;

  if (!fix_valid) {
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

#else /* CONFIG_ASSET_TRACKER_SIM_GPS */

/* Simulated GNSS: fabricates a small circular track around a configurable
 * base coordinate, computed fresh from k_uptime_get() each call -- no real
 * modem/GNSS interaction at all. See prj.conf/Kconfig for why this exists:
 * GNSS indoors will very likely never get a real fix, and this lets the
 * telemetry pipeline + dashboard be demonstrated anyway. */

#include <math.h>
#include <stdlib.h>

#define PI 3.14159265358979323846
#define EARTH_RADIUS_M (6371.0 * 1000.0)

/* Simulated satellite count is a fixed, plausible-looking constant -- not
 * meant to model real sky visibility, just to look like "GNSS is working"
 * on a dashboard exercising this demo path. */
#define SIM_SATS 9

static bool base_parsed;
static double base_lat_rad;
static double base_lat;
static double base_lon;

static void ensure_base_parsed(void) {
  if (base_parsed) {
    return;
  }

  base_lat = strtod(CONFIG_ASSET_TRACKER_SIM_BASE_LAT, NULL);
  base_lon = strtod(CONFIG_ASSET_TRACKER_SIM_BASE_LON, NULL);
  base_lat_rad = base_lat * PI / 180.0;
  base_parsed = true;

  LOG_WRN(
      "SIMULATED GPS mode enabled -- reporting a fabricated %dm circuit around "
      "%.6f,%.6f every %ds lap, NOT a real fix",
      CONFIG_ASSET_TRACKER_SIM_RADIUS_M, base_lat, base_lon, CONFIG_ASSET_TRACKER_SIM_PERIOD_SEC
  );
}

int tracker_gnss_init(void) {
  ensure_base_parsed();
  return 0;
}

void tracker_gnss_get_latest(struct tracker_position *out) {
  ensure_base_parsed();

  double elapsed_s = (double)k_uptime_get() / 1000.0;
  double period_s = (double)CONFIG_ASSET_TRACKER_SIM_PERIOD_SEC;
  double angle = 2.0 * PI * fmod(elapsed_s, period_s) / period_s;
  double radius_m = (double)CONFIG_ASSET_TRACKER_SIM_RADIUS_M;

  /* Small-circuit flat-earth approximation -- plenty accurate for a
   * "plausible moving track" demo at this radius, not meant for anything
   * requiring real geodesy. */
  double lat_offset_deg = (radius_m / EARTH_RADIUS_M) * (180.0 / PI) * cos(angle);
  double lon_offset_deg =
      (radius_m / EARTH_RADIUS_M) * (180.0 / PI) * sin(angle) / cos(base_lat_rad);

  out->latitude = base_lat + lat_offset_deg;
  out->longitude = base_lon + lon_offset_deg;
  /* A gentle sinusoidal bob so altitude isn't perfectly flat -- arbitrary,
   * just enough to look like real noisy GNSS altitude on a graph. */
  out->altitude_m = 50.0f + (float)(3.0 * sin(angle));
  out->speed_mps = (float)(2.0 * PI * radius_m / period_s);
  /* Heading of travel around the circle, 0 = north, clockwise -- the
   * derivative of position angle, offset 90 degrees since motion is
   * tangent to the radius. */
  out->heading_deg = fmodf((float)(angle * 180.0 / PI) + 90.0f, 360.0f);
  out->sats = SIM_SATS;
  out->fix_quality = TRACKER_FIX_SIMULATED;
}

#endif /* CONFIG_ASSET_TRACKER_SIM_GPS */
