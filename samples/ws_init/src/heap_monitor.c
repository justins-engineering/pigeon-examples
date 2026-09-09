/** @headerfile heap_monitor.h */
#include "heap_monitor.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_heap.h>

LOG_MODULE_REGISTER(heap_monitor);

/* The pool behind CONFIG_HEAP_MEM_POOL_SIZE, which a WiFi driver's receive
 * buffers and the network buffer pools draw from. No public header declares
 * it; the shell's own "kernel heap" command reads the same object this way. */
extern struct k_heap _system_heap;

#if defined(CONFIG_COMMON_LIBC_MALLOC) && CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE != 0
#define HEAP_MONITOR_LIBC 1
/* A second, separate arena: a vendor blob calling plain malloc() lands here
 * whatever the kernel-heap Kconfig says, so a crash in one is invisible in
 * the other. Declared here for the same reason as _system_heap above. */
extern int malloc_runtime_stats_get(struct sys_memory_stats* stats);
#endif

/* Frequent enough to catch a slide inside a short soak, sparse enough that a
 * multi-hour log stays greppable. */
#define HEAP_MONITOR_INTERVAL K_SECONDS(30)

static struct k_work_delayable heap_monitor_work;

static void heap_monitor_handler(struct k_work* work) {
  ARG_UNUSED(work);

  /* Uptime on every line, so the trend can be plotted from the console log
   * alone on a device with no wall clock. */
  uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);

  struct sys_memory_stats stats;
  int err = sys_heap_runtime_stats_get(&_system_heap.heap, &stats);

  if (err) {
    LOG_ERR("sys_heap_runtime_stats_get failed: %d", err);
  } else {
    LOG_INF(
        "heap_stats uptime_s=%u free=%zu allocated=%zu max_allocated=%zu", uptime_s,
        stats.free_bytes, stats.allocated_bytes, stats.max_allocated_bytes
    );
  }

#if defined(HEAP_MONITOR_LIBC)
  struct sys_memory_stats libc_stats;
  int libc_err = malloc_runtime_stats_get(&libc_stats);

  if (libc_err) {
    LOG_ERR("malloc_runtime_stats_get failed: %d", libc_err);
  } else {
    LOG_INF(
        "libc_heap_stats uptime_s=%u free=%zu allocated=%zu max_allocated=%zu", uptime_s,
        libc_stats.free_bytes, libc_stats.allocated_bytes, libc_stats.max_allocated_bytes
    );
  }
#endif

  k_work_reschedule(&heap_monitor_work, HEAP_MONITOR_INTERVAL);
}

void heap_monitor_start(void) {
  k_work_init_delayable(&heap_monitor_work, heap_monitor_handler);
  k_work_reschedule(&heap_monitor_work, HEAP_MONITOR_INTERVAL);
}
