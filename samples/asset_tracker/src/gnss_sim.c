/** @headerfile gnss.h
 *  A fabricated circuit around a configured coordinate, for a board with no
 *  receiver and for an indoor Feather where a fix will never come.
 */
#include "gnss.h"

#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss, CONFIG_ASSET_TRACKER_LOG_LEVEL);

/* Picolibc leaves M_PI out under a strict C standard. */
#define PI 3.14159265358979323846
#define EARTH_RADIUS_M 6371000.0

/* Plausible and fixed: the track models motion, not sky visibility. */
#define SIM_SATS 9

static double base_lat;
static double base_lon;
static double base_lat_rad;

int tracker_gnss_init(void) {
  base_lat = strtod(CONFIG_ASSET_TRACKER_SIM_BASE_LAT, NULL);
  base_lon = strtod(CONFIG_ASSET_TRACKER_SIM_BASE_LON, NULL);
  base_lat_rad = base_lat * PI / 180.0;

  LOG_INF(
      "Simulated track: a %d m circuit around %.6f,%.6f, one lap every %d s",
      CONFIG_ASSET_TRACKER_SIM_RADIUS_M, base_lat, base_lon,
      CONFIG_ASSET_TRACKER_SIM_PERIOD_SEC
  );

  return 0;
}

void tracker_gnss_get_latest(struct tracker_position* out) {
  double elapsed_s = (double)k_uptime_get() / 1000.0;
  double period_s = (double)CONFIG_ASSET_TRACKER_SIM_PERIOD_SEC;
  double angle = 2.0 * PI * fmod(elapsed_s, period_s) / period_s;
  double radius_m = (double)CONFIG_ASSET_TRACKER_SIM_RADIUS_M;

  /* A flat-earth offset, which at this radius is indistinguishable from the
   * geodesic one. */
  double lat_offset_deg = (radius_m / EARTH_RADIUS_M) * (180.0 / PI) * cos(angle);
  double lon_offset_deg =
      (radius_m / EARTH_RADIUS_M) * (180.0 / PI) * sin(angle) / cos(base_lat_rad);

  out->latitude = base_lat + lat_offset_deg;
  out->longitude = base_lon + lon_offset_deg;
  /* A gentle bob, so a graph of altitude is not a flat line. */
  out->altitude_m = 50.0f + (float)(3.0 * sin(angle));
  out->speed_mps = (float)(2.0 * PI * radius_m / period_s);
  /* Motion is tangent to the radius, hence the quarter turn. */
  out->heading_deg = fmodf((float)(angle * 180.0 / PI) + 90.0f, 360.0f);
  out->sats = SIM_SATS;
  out->fix_quality = TRACKER_FIX_SIMULATED;
}
