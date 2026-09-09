/** @file shadow.h
 *  @brief Shadow sync: fetch, apply, report back, plus telemetry.
 */
#ifndef SHADOW_H
#define SHADOW_H

/** Fetches the shadow, applies a newer target_config and reports the result.
 *  Returns 0 whether or not anything changed, negative on a transport or
 *  parse failure. */
int shadow_sync(void);

/** Runs shadow_sync() forever, sleeping the shadow's own telemetry_interval
 *  between polls. */
void shadow_loop(void);

#endif
