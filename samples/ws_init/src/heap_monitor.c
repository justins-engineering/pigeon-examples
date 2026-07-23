/** @headerfile heap_monitor.h */
#include "heap_monitor.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_heap.h>

LOG_MODULE_REGISTER(heap_monitor);

/* CONFIG_HEAP_MEM_POOL_SIZE's backing sys_heap object (zephyr/kernel/mempool.c) --
 * not declared in any public Zephyr header, so this `extern` mirrors the one the
 * built-in "kernel heap" shell command
 * (zephyr/subsys/shell/modules/kernel_service/heap.c) uses to read the same stats. */
extern struct k_heap _system_heap;

/* A SEPARATE heap from _system_heap above: CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE's
 * private arena backing plain malloc()/calloc()/free() (zephyr/lib/libc/common/
 * source/stdlib/malloc.c), sized from the linker's _end symbol up to
 * _heap_sentry -- essentially whatever SRAM is left over after every other
 * static allocation. esp_wifi's own OS-adapter layer (esp_heap_adapter.h) is
 * Kconfig'd to draw from _system_heap instead when CONFIG_ESP_WIFI_HEAP_SYSTEM=y
 * (the case here), but the closed-source Espressif MAC-layer blob this board
 * vendors (zephyr/blobs/lib/esp32c6/libnet80211.a -- no source in this tree,
 * so it can't respect that Kconfig choice) calls plain malloc()/free() directly
 * for its own timer/beacon/probe housekeeping, which can only ever come from
 * THIS arena. A real-hardware crash traced to exactly that path (task #15,
 * see this sample's README/CLAUDE.md writeup) is why this second heap is
 * tracked here too, not just _system_heap -- malloc_runtime_stats_get() is
 * this file's own public wrapper around sys_heap_runtime_stats_get() for its
 * otherwise-static z_malloc_heap, gated on the same CONFIG_SYS_HEAP_RUNTIME_
 * STATS already enabled for _system_heap, but (like _system_heap) not declared
 * in any public header. */
extern int malloc_runtime_stats_get(struct sys_memory_stats *stats);

/* 30s: frequent enough to catch a fast leak/fragmentation slide within a
 * reasonably short soak window, infrequent enough that a multi-hour soak's
 * serial log stays a manageable size to grep/plot afterward. */
#define HEAP_MONITOR_INTERVAL K_SECONDS(30)

static struct k_work_delayable heap_monitor_work;

static void heap_monitor_handler(struct k_work *work) {
  ARG_UNUSED(work);

  uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);

  struct sys_memory_stats stats;
  int err = sys_heap_runtime_stats_get(&_system_heap.heap, &stats);

  if (err) {
    LOG_ERR("sys_heap_runtime_stats_get failed: %d", err);
  } else {
    /* uptime alongside every sample so a serial-log grep can plot free
     * bytes against time without relying on a wall-clock timestamp --
     * task #15 wants exactly this trend over a long soak. */
    LOG_INF("heap_stats uptime_s=%u free=%zu allocated=%zu max_allocated=%zu",
            uptime_s, stats.free_bytes, stats.allocated_bytes, stats.max_allocated_bytes);
  }

  struct sys_memory_stats libc_stats;
  int libc_err = malloc_runtime_stats_get(&libc_stats);

  if (libc_err) {
    LOG_ERR("malloc_runtime_stats_get failed: %d", libc_err);
  } else {
    /* See the extern declaration's comment above -- this is the arena the
     * closed-source WiFi MAC blob's own malloc()/free() calls actually draw
     * from, distinct from heap_stats above. */
    LOG_INF("libc_heap_stats uptime_s=%u free=%zu allocated=%zu max_allocated=%zu", uptime_s,
            libc_stats.free_bytes, libc_stats.allocated_bytes, libc_stats.max_allocated_bytes);
  }

  k_work_reschedule(&heap_monitor_work, HEAP_MONITOR_INTERVAL);
}

void heap_monitor_start(void) {
  k_work_init_delayable(&heap_monitor_work, heap_monitor_handler);
  k_work_reschedule(&heap_monitor_work, HEAP_MONITOR_INTERVAL);
}
