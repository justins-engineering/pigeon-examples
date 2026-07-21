/** @file shadow.h
 *  @brief Fetches the device shadow from the platform and applies it locally.
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <pigeon.h>

#if defined(CONFIG_PIGEON_WS)
/** @fn void shadow_ws_event_cb(enum pigeon_ws_event, const struct pigeon_shadow_doc *)
 *  @brief pigeon_ws_start() event callback: both PIGEON_WS_EVENT_CONNECTED
 *  and PIGEON_WS_EVENT_SHADOW_UPDATE just wake shadow_loop() to re-sync now
 *  instead of waiting out its telemetry_interval sleep -- see shadow_loop()'s
 *  doc for why this is a wakeup, not a data path, in v1.
 */
void shadow_ws_event_cb(enum pigeon_ws_event ev, const struct pigeon_shadow_doc *shadow);
#endif

/** @fn int shadow_sync(void)
 *  @brief Fetch the device shadow, apply target_config if the platform has
 *  moved to a newer version than what's already applied this boot, and log
 *  what changed.
 *  @return 0 on success (whether or not an update was applied), negative
 *  error code on transport/parse failure.
 */
int shadow_sync(void);

/** @fn void shadow_loop(void)
 *  @brief Repeatedly shadow_sync(), waiting the shadow's own
 *  telemetry_interval (seconds) between polls -- config polling during
 *  what would be a telemetry publish window, once telemetry publishing
 *  exists (see pigeon's CLAUDE.md notes on missing backend endpoints).
 *  When CONFIG_PIGEON_WS is enabled, the wait is a semaphore take rather
 *  than a plain sleep, so a pushed shadow_update (or a fresh WS connect)
 *  wakes this loop immediately instead of waiting out the interval --
 *  telemetry_interval remains the safety-net poll period while the WS
 *  socket is down. Never returns under normal operation.
 */
void shadow_loop(void);

#endif
