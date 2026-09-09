/** @file heap_monitor.h
 *  @brief Periodic heap logging, so a soak shows whether free memory trends
 *  down (a leak), stays flat while allocations still fail (fragmentation) or
 *  simply holds.
 */
#ifndef HEAP_MONITOR_H
#define HEAP_MONITOR_H

/** Starts the periodic log. Call once, after the network and the library are
 *  up, so the first sample is steady state rather than boot churn. */
void heap_monitor_start(void);

#endif
