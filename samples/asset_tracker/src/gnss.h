/** @file gnss.h
 *  @brief The tracker's position source: the nRF91's own GNSS receiver, or
 *  the simulated track every board without one reports.
 */
#ifndef GNSS_H
#define GNSS_H

/** Reported as the gps_fix_quality telemetry key. */
enum tracker_fix_quality {
  /** No valid fix yet, the expected steady state indoors. */
  TRACKER_FIX_NONE = 0,
  TRACKER_FIX_REAL = 1,
  /** A fabricated position, so a dashboard never reads it as a real fix. */
  TRACKER_FIX_SIMULATED = 2,
};

/** One position sample, real or simulated. */
struct tracker_position {
  double latitude;
  double longitude;
  float altitude_m;
  float speed_mps;
  float heading_deg;
  /** Satellites tracked, whether or not each was used in the fix; a receiver
   *  still searching reports these with no position. */
  int sats;
  enum tracker_fix_quality fix_quality;
};

/** Starts the position source. Call it once the network is up: the modem only
 *  accepts the GNSS functional mode with its library already running. */
int tracker_gnss_init(void);

/** Fills out with the latest sample. A real receiver leaves it at
 *  TRACKER_FIX_NONE until it fixes; the simulated track always has one. */
void tracker_gnss_get_latest(struct tracker_position* out);

#endif
