/** @file gnss.h
 *  @brief Position source for the asset tracker: real nRF91 GNSS fixes, or
 *  (CONFIG_ASSET_TRACKER_SIM_GPS) a synthetic moving track for indoor/CI
 *  demos where a real fix will never come.
 */
#ifndef GNSS_H
#define GNSS_H

/** Reported as the numeric gps_fix_quality telemetry key -- see shadow.c. */
enum tracker_fix_quality {
  /** No valid GNSS fix has been produced yet this boot (or the current one
   *  expired) -- expected indoors, and reported honestly rather than
   *  omitted so a dashboard can show "searching" instead of "no data". */
  TRACKER_FIX_NONE = 0,
  /** A real GNSS fix (NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID was set). */
  TRACKER_FIX_REAL = 1,
  /** CONFIG_ASSET_TRACKER_SIM_GPS's synthetic track, not a real fix --
   *  always reported as this code, never as TRACKER_FIX_REAL, so a real
   *  fix and a simulated demo can never be mistaken for each other on the
   *  dashboard. */
  TRACKER_FIX_SIMULATED = 2,
};

/** One reported position sample, real or simulated. */
struct tracker_position {
  double latitude;
  double longitude;
  float altitude_m;
  float speed_mps;
  float heading_deg;
  /** Satellites currently tracked (real mode: sv[] entries with a nonzero
   *  SV id, whether or not they ended up used in the fix; simulated mode:
   *  a fixed plausible constant). Meaningful even when fix_quality is
   *  TRACKER_FIX_NONE -- see tracker_gnss_get_latest(). */
  int sats;
  enum tracker_fix_quality fix_quality;
};

/**
 * @brief Start the position source. Call once, after pigeon_init().
 *
 * Real-GNSS mode: registers the nrf_modem_gnss event handler and starts
 * periodic navigation (CONFIG_ASSET_TRACKER_GNSS_FIX_INTERVAL_SEC /
 * _FIX_RETRY_SEC). Requires the modem's system mode to include GPS (see
 * CONFIG_LTE_NETWORK_MODE_LTE_M_GPS in prj.conf) -- that's what lets the
 * modem interleave GNSS search with LTE reception on its own, with no
 * manual connect/disconnect dance from this sample.
 *
 * Simulated mode (CONFIG_ASSET_TRACKER_SIM_GPS): does not touch the modem
 * or GNSS hardware at all; always returns 0.
 *
 * @return 0 on success, negative errno on failure to start real GNSS.
 */
int tracker_gnss_init(void);

/**
 * @brief Fill out with the most recently available position sample.
 *
 * Real-GNSS mode: the last PVT notification received from the modem, if
 * any -- may have fix_quality TRACKER_FIX_NONE (sats/whatever was tracked
 * still filled in) if GNSS hasn't produced a valid fix yet, which is the
 * expected steady state indoors. If no PVT has ever been received this
 * boot, every field is zeroed and fix_quality is TRACKER_FIX_NONE.
 *
 * Simulated mode: always fabricates a fresh sample for "now" along the
 * configured circuit -- never "no data".
 */
void tracker_gnss_get_latest(struct tracker_position *out);

#endif
