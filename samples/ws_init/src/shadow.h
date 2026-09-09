/** @file shadow.h
 *  @brief Shadow sync: fetch, apply, report back, plus telemetry.
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <pigeon.h>

/** Wakes shadow_loop() so a pushed shadow is applied without waiting out the
 *  poll interval. Runs on the WebSocket worker thread, so it only signals. */
void shadow_ws_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc* shadow);

/** Fetches the shadow, applies a newer target_config and reports the result.
 *  Returns 0 whether or not anything changed, negative on a transport or
 *  parse failure. */
int shadow_sync(void);

/** Runs shadow_sync() forever, waiting the shadow's own telemetry_interval
 *  between polls or until the platform pushes an update. */
void shadow_loop(void);

#endif
