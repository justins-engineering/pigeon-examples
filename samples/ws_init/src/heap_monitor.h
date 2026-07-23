/** @file heap_monitor.h
 *  @brief PidgeIoT task #15 instrumentation: periodic logging of the system
 *  heap (CONFIG_HEAP_MEM_POOL_SIZE, the pool backing k_malloc() and, per
 *  prj.conf's comment on that Kconfig symbol, the WiFi driver's own static/
 *  dynamic RX buffers and net_buf/net_pkt pools) so a real-hardware soak can
 *  show whether free bytes trend down over uptime (leak), stay flat while
 *  the "esp32c6_wifi_adapter: memory allocation failed" warning still fires
 *  (fragmentation -- aggregate free space exists but no single chunk is big
 *  enough), or the warning simply stops recurring (prior sizing round
 *  arrested it). Not wired into wifi_init: that sample has never been run on
 *  real hardware at all yet, so there's no prior warning history there to
 *  compare a soak against -- see this repo's CLAUDE.md.
 */
#ifndef HEAP_MONITOR_H
#define HEAP_MONITOR_H

/** @fn void heap_monitor_start(void)
 *  @brief Starts periodic system-heap usage logging (see file header). Call
 *  once, after wifi_connect()/pigeon_init() succeed -- see main.c. Logs at
 *  LOG_INF, so CONFIG_LOG_DEFAULT_LEVEL=3 (already set) is enough to see it
 *  on the serial console without any other Kconfig change.
 */
void heap_monitor_start(void);

#endif
