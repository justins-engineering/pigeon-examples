# DRAFT — not posted. For Justin to review and file at
# https://github.com/zephyrproject-rtos/hal_espressif/issues

**Title suggestion:** ESP32-C6: `wifi` thread hard-panics inside `libnet80211.a`
(`mgd_probe_send_timeout` → allocator ecall) after 40-60 min of WiFi station
uptime with reconnect churn

## Summary

On an ESP32-C6-DevKitC-1 running a Zephyr application with the native
WiFi/mbedTLS stack (no cellular/modem offload), the `wifi` thread eventually
hits a fatal kernel panic (`ZEPHYR FATAL ERROR 4`, `Environment call from
M-mode` — i.e. a deliberate `ecall`-based assert/fault path, not a wild
pointer dereference) after roughly 40-60 minutes of continuous WiFi station
uptime under normal application traffic (periodic HTTPS requests + a
persistent WebSocket connection with reconnect churn). The panic's call
stack resolves into `libnet80211.a`'s closed-source WiFi MAC-layer
timer/probe/beacon-management code, calling into the C library allocator.
Two independent hardware soaks reproduced the same class of failure (first
at ~42 min, second — unconfirmed exact trace, see below — apparently again
around/after ~60 min).

This is reported against `hal_espressif` because the crashing code
(`wl_cnx.o`, `ieee80211_timer.o`) lives in the vendored, source-unavailable
`zephyr/blobs/lib/esp32c6/libnet80211.a` blob this module ships — there is no
source in the Zephyr tree to fix directly, and it isn't obviously a bug in
the application, `pigeon` (our device library), or Zephyr's own kernel/heap
code.

## Environment

- Board: ESP32-C6-DevKitC-1 (`esp32c6_devkitc/esp32c6/hpcore`)
- Zephyr: 4.4.1 (vanilla upstream manifest, no NCS/sdk-nrf)
- `hal_espressif` revision: `b7953b8019361d09e613f7011d2ccc41b984d087`
- Application: a Zephyr sample using Zephyr's native `CONFIG_WIFI`
  (`esp_adapter.c`/`libnet80211.a`) station mode + `CONFIG_NET_SOCKETS_SOCKOPT_TLS`
  (mbedTLS) for HTTPS, plus `CONFIG_WEBSOCKET_CLIENT` for a persistent `wss://`
  connection with a device-owned ~60s ping/pong keepalive and automatic
  reconnect-with-backoff on any drop.
- Traffic pattern: periodic HTTPS GET/POST (shadow poll + telemetry, roughly
  once every 10-60s depending on config) concurrent with the persistent WS
  socket's keepalive/telemetry traffic. Real WiFi AP, real internet-routed
  HTTPS/WSS endpoint (not `native_sim`/loopback).

## Symbol-resolved crash trace (round 1, uptime 2502s / ~41.7min)

Resolved via `riscv64-zephyr-elf-addr2line -f -C` against the build's `zephyr.elf`:

```
Current thread: 0x40831598 (wifi)
mcause: 11, Environment call from M-mode
mtval: 73

call trace:
  0: mepc=0x42022328 -> (nearest symbol) malloc_prepare
                         zephyr/lib/libc/common/source/stdlib/malloc.c:236
                         -- NOTE: malloc_prepare() itself is a SYS_INIT-only
                         function with no asserts in this build's config
                         (CONFIG_COMMON_LIBC_MALLOC, non-ALLOCATE_HEAP_AT_STARTUP
                         branch), so this is almost certainly a nearest-symbol
                         misattribution -- the real fault is very likely inside
                         malloc()/sys_heap_aligned_alloc() nearby in the same
                         translation unit, not literally in malloc_prepare().
  1: ra=0x4206b7e8    -> mgd_probe_send_timeout   (wl_cnx.o, libnet80211.a)
  2: ra=0x42015a8e    -> ets_timer_setfn          (esp_timer/src/ets_timer_legacy.c:62)
  3: ra=0x4206b7e8    -> mgd_probe_send_timeout   (wl_cnx.o, libnet80211.a)
  4: ra=0x4206e28a    -> cnx_beacon_timeout_process (??:?, libnet80211.a)
  5: ra=0x420695f6    -> ieee80211_beacon         (ieee80211_timer.o, libnet80211.a)
  6: ra=0x42069880    -> ieee80211_timer_do_process (??:?, libnet80211.a)
  7: ra=0x42002ba0    -> ppTask                   (??:?, libnet80211.a -- the WiFi
                                                    driver's own top-level task,
                                                    matching "Current thread: wifi")
```

After the panic, Zephyr's default `k_sys_fatal_error_handler()` /
`arch_system_halt()` runs (`arch_irq_lock()` + spin forever) — confirmed on
real hardware as total serial console silence and zero response to shell
input, for over an hour, on both reproductions. This is expected/correct
Zephyr behavior given the panic; it's mentioned only so a reader
understands why the device never "comes back" from this on its own (see the
separate watchdog-mitigation work referenced below).

## Repro profile / conditions correlated with the failure

- **Not an immediate/deterministic failure.** Round 1 crashed at ~42 min
  uptime; round 2 (same firmware, same build) ran for 60+ clean minutes with
  no failure signal at all before going silent (a second wedge is suspected
  but its exact trace wasn't captured — logging infrastructure issue on our
  end, not a hardware issue). Timing to failure appears to vary with
  real-world WiFi/AP conditions (this workspace's own `CLAUDE.md` separately
  documents flaky, undiagnosed WiFi-join timing variance run-to-run on this
  same board).
- **Correlates with reconnect/retry churn**, not just raw uptime: recurring
  `esp32c6_wifi_adapter: memory allocation failed` (from `esp_adapter.c`'s
  `wifi_malloc`/`wifi_calloc`, when `CONFIG_ESP_WIFI_HEAP_SYSTEM=y` — these
  draw from Zephyr's own `k_malloc`/`CONFIG_HEAP_MEM_POOL_SIZE` pool, not the
  arena the panic traces into) and `esp32_wifi: Failed to send packet`
  errors preceded the panic in both runs, alongside eventual application-
  level HTTPS/WS connect timeouts (`-116`).
- **Ruled out: heap exhaustion.** We instrumented both plausible heap pools
  with `sys_heap_runtime_stats_get()`/`malloc_runtime_stats_get()`:
  - `CONFIG_HEAP_MEM_POOL_SIZE` (48KiB, backs `k_malloc`, and per our own
    prior sizing work is what `esp_adapter.c`'s `wifi_malloc`/`wifi_calloc`
    draw from when `CONFIG_ESP_WIFI_HEAP_SYSTEM=y`): showed a real, slow,
    reproducible decline in its *recovered* floor across both soaks
    (roughly 60-75 bytes/min), but still had ~14KB free at the time of the
    round-1 crash — not exhausted.
  - `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` (~151.7KB, backs plain
    `malloc()`/`free()` — the pool the panic's call stack actually traces
    into, since `libnet80211.a`'s MAC-layer blob calls libc `malloc()`
    directly and can't respect the `CONFIG_ESP_WIFI_HEAP_SYSTEM` Kconfig
    choice that only rewires the source-level `esp_adapter.c` glue):
    showed `allocated=0, max_allocated=0` for the **entire 60+ clean minutes**
    of round 2 — i.e. this arena was never touched at all during normal
    operation, ruling out both "it leaks" and "it's undersized" as
    explanations for the eventual failure.

  Net conclusion: this does not look like a resource-exhaustion bug in
  either Zephyr-side heap. It looks like a rare, conditionally-triggered
  fault inside the vendored MAC-layer timer/probe/beacon-management code
  itself (`libnet80211.a`), plausibly provoked by — but not simply caused
  by — memory/retry pressure elsewhere in the WiFi stack during reconnect
  churn.

## What we've done about it (not a fix for this issue, for context)

Since the fault lives in a closed-source blob this repo has no source for,
we did not attempt to patch `libnet80211.a` itself. Instead we added an
application-level mitigation: a Zephyr Task Watchdog (`CONFIG_TASK_WDT`)
channel fed by confirmed application-level liveness (WS pong receipt,
successful telemetry/shadow round trips), with the board's hardware
watchdog as a fallback, so the device self-recovers via `sys_reboot()`
rather than hanging forever. That work also surfaced what looks like a
**separate**, likely-Zephyr-side bug in `drivers/watchdog/wdt_esp32.c`
(missing ms→ticks conversion) — reported separately, see
`zephyr-wdt_esp32-missing-tick-conversion.md` in this same directory.

## Ask

Any guidance on:
1. Whether this is a known issue with `libnet80211.a`'s beacon/probe timeout
   handling under WiFi churn on ESP32-C6 specifically, or reproducible on
   other ESP32 variants sharing this blob.
2. Whether a newer `hal_espressif`/blob revision has a fix, or whether
   there's a recommended Kconfig-level mitigation (e.g. disabling active
   probe scanning, adjusting beacon-loss thresholds) short of the
   application-level watchdog workaround described above.
