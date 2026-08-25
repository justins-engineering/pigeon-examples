/** @file shadow.h
 *  @brief Applies the pigeon's target shadow, which on this connector is
 *  pushed by the broker rather than polled for.
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <pigeon.h>

/** @fn void shadow_event_cb(enum pigeon_event, const struct pigeon_shadow_doc *)
 *  @brief pigeon_mqtt_start() event callback: a connect or a pushed shadow
 *  wakes shadow_loop() to apply it now instead of at the next tick. Called
 *  from pigeon's MQTT worker thread, so it does no work of its own beyond
 *  signalling.
 */
void shadow_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc *shadow);

/** @fn int shadow_sync(void)
 *  @brief Read the pigeon's target shadow, apply target_config if the
 *  platform has moved past what this boot already applied, report the
 *  result back, and report telemetry.
 *  @return 0 on success (whether or not an update was applied), negative
 *  error code on transport/parse failure.
 */
int shadow_sync(void);

/** @fn void shadow_loop(void)
 *  @brief Repeatedly shadow_sync(), waiting the shadow's own
 *  telemetry_interval (seconds) between passes -- or less, when the broker
 *  pushes a new target. Never returns under normal operation.
 */
void shadow_loop(void);

#endif
