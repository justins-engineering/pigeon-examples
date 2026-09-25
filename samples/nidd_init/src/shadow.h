/** @file shadow.h
 *  @brief Applies the pigeon's target shadow, which over NIDD arrives as the
 *  reply to an uplink or as a push, and paces the device's radio use.
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <pigeon.h>

/** pigeon_nidd_start() event callback: a newer shadow wakes shadow_loop() to
 *  apply it on this connection instead of at the next wake. Runs on the
 *  library's receive thread, which also receives the reply a report waits
 *  for, so it only signals. */
void shadow_event_cb(enum pigeon_event ev, const struct pigeon_shadow_doc* shadow);

/** Applies target_config if the platform has moved past what this boot
 *  applied, and reports the applied version while the platform holds an older
 *  one; a report that went unconfirmed is sent again at the next wake.
 *  @return 0 whether or not an update was applied, negative when no shadow is
 *  available. */
int shadow_sync(void);

/** Takes a reading every quarter of the wake interval and sends the four as
 *  one batch at each wake, syncing the shadow on the same connection. Does not
 *  return under normal operation. */
void shadow_loop(void);

#endif
