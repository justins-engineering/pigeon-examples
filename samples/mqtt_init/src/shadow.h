/** @file shadow.h
 *  @brief Applies the pigeon's target shadow, which on this connector the
 *  broker pushes rather than the device polling for it.
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <pigeon.h>

/** pigeon_mqtt_start() event callback: a connect or a pushed shadow wakes
 *  shadow_loop() to apply it now instead of at the next tick. Runs on the
 *  library's MQTT worker thread, so it only signals. */
void shadow_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc *shadow);

/** Reads the pigeon's target shadow, applies target_config if the platform
 *  has moved past what this boot applied, reports the result back and reports
 *  telemetry.
 *  @return 0 whether or not an update was applied, negative on transport or
 *  parse failure. */
int shadow_sync(void);

/** Repeatedly shadow_sync(), waiting the shadow's own telemetry_interval
 *  between passes or less when the broker pushes a new target. Does not
 *  return under normal operation. */
void shadow_loop(void);

#endif
