/** @file shadow.h
 *  @brief Fetches the device shadow from the platform and applies it locally.
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <pigeon.h>

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
 *  Always a plain sleep between polls: this sample deliberately has no
 *  persistent push channel (see ws_init for that), so telemetry_interval
 *  is the only thing that ever wakes this loop early. Never returns under
 *  normal operation.
 */
void shadow_loop(void);

#endif
