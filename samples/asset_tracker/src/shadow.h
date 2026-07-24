/** @file shadow.h
 *  @brief Fetches the device shadow from the platform, applies it locally,
 *  and reports uptime + GNSS position telemetry each poll.
 */
#ifndef SHADOW_H
#define SHADOW_H

/** @fn int shadow_sync(void)
 *  @brief Fetch the device shadow, apply target_config if the platform has
 *  moved to a newer version than what's already applied this boot, report
 *  uptime + the latest GNSS position (see gnss.h) as telemetry, and log
 *  what changed.
 *  @return 0 on success (whether or not an update was applied), negative
 *  error code on transport/parse failure.
 */
int shadow_sync(void);

/** @fn void shadow_loop(void)
 *  @brief Repeatedly shadow_sync(), sleeping the shadow's own
 *  telemetry_interval (seconds) between polls. Never returns under normal
 *  operation.
 */
void shadow_loop(void);

#endif
