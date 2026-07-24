# DRAFT — not posted. For Justin to review and file at
# https://github.com/zephyrproject-rtos/zephyr/issues

**Title suggestion:** `drivers/watchdog/wdt_esp32.c`: `wdt_timeout_cfg.window.max`
(milliseconds, per the generic WDT API) passed directly to
`wdt_hal_config_stage()` as raw ticks — no ms→ticks conversion

## Confidence note (read first)

The code-level mismatch (below) is unambiguous: a documented-as-ticks
parameter is fed a millisecond value with no conversion, and a sibling
file in the same tree demonstrates the conversion that's missing. What's
LOWER confidence is the real-world consequence: two hardware attempts both
saw no hardware-reset recovery at all (up to a firm 9.2-minute lower bound,
see "Empirical observation"), which is consistent with the hypothesis but
doesn't pin down whether the effective deadline is merely much longer than
intended or whether the reset action isn't firing in this configuration at
all — we didn't have time to distinguish the two. Treat the root-cause
diagnosis as solid and the exact real-world magnitude/whether it ever fires
as open questions for whoever picks this up.

## Summary

`zephyr/drivers/watchdog/wdt_esp32.c`'s `wdt_esp32_set_config()`:

```c
static int wdt_esp32_set_config(const struct device *dev, uint8_t options)
{
	struct wdt_esp32_data *data = dev->data;

	wdt_esp32_unseal(dev);
	wdt_hal_config_stage(&data->hal, WDT_STAGE0, data->timeout, WDT_STAGE_ACTION_INT);
	wdt_hal_config_stage(&data->hal, WDT_STAGE1, data->timeout, data->mode);
	...
```

`data->timeout` is set in `wdt_esp32_install_timeout()` directly from
`cfg->window.max`, i.e. Zephyr's generic `struct wdt_timeout_cfg` field,
which is documented (`include/zephyr/drivers/watchdog.h`) and universally
treated elsewhere as **milliseconds**.

But `wdt_hal_config_stage()`'s own parameter doc
(`modules/hal/espressif/components/esp_hal_wdt/include/hal/wdt_hal.h:77`)
says:

```
 * @param timeout Number of WDT ticks for the stage to time out
```

i.e. it expects **hardware ticks**, not milliseconds — and this driver never
converts between the two. The file even defines the constants that would be
needed to do so (`MWDT_TICK_PRESCALER`, `MWDT_TICKS_PER_US`) but never
actually uses them anywhere in the file (confirmed via grep — they only
appear in their own `#define` lines).

## Evidence the conversion is required (same vendored tree)

ESP-IDF's own reference Task Watchdog implementation, vendored in the exact
same `hal_espressif` module tree
(`modules/hal/espressif/components/esp_system/task_wdt/task_wdt_impl_timergroup.c`),
calls the identical `wdt_hal_config_stage()` API and — unlike
`wdt_esp32.c` — *does* convert:

```c
wdt_hal_config_stage(&ctx->hal, WDT_STAGE0, config->timeout_ms * (1000 / TWDT_TICKS_PER_US), WDT_STAGE_ACTION_INT);
wdt_hal_config_stage(&ctx->hal, WDT_STAGE1, config->timeout_ms * (2 * 1000 / TWDT_TICKS_PER_US), WDT_STAGE_ACTION_RESET_SYSTEM);
```

This is about as close to a same-repo, side-by-side "correct usage vs.
missing conversion" comparison as you can get.

## Impact

Any Zephyr application relying on `CONFIG_WDT_ESP32` (directly, or via
`CONFIG_TASK_WDT` + `CONFIG_TASK_WDT_HW_FALLBACK`, which installs a hardware
timeout of `CONFIG_TASK_WDT_MIN_TIMEOUT + CONFIG_TASK_WDT_HW_FALLBACK_DELAY`
milliseconds) is getting a hardware watchdog deadline that does not actually
correspond to the millisecond value it configured — the real deadline is
whatever `<that ms value> ticks` (unconverted) works out to in real time
given this SoC's WDT prescaler/clock, which is very unlikely to be the
intended duration.

## Empirical observation (real hardware, ESP32-C6-DevKitC-1)

While building an application-level watchdog on top of `CONFIG_TASK_WDT` +
`CONFIG_TASK_WDT_HW_FALLBACK` (default config: `CONFIG_TASK_WDT_MIN_TIMEOUT`
=100, `CONFIG_TASK_WDT_HW_FALLBACK_DELAY`=20, i.e. an intended ~120ms
hardware deadline), we deliberately forced a genuine interrupts-disabled
hang (`arch_irq_lock(); for (;;) {}` — reproducing Zephyr's own default
`arch_system_halt()` behavior) to verify the hardware fallback would recover
the device, across two separate attempts:

- **Attempt 1**: no recovery within an 88+ second observation window (~700x
  the intended ~120ms deadline) before we moved on.
- **Attempt 2** (fresh boot, same image, longer observation): a continuous
  live serial capture confirmed **zero bytes received for 550+ consecutive
  seconds** (~9.2 minutes, ~4500x the intended deadline) after the hang
  began, with no reset/reboot observed in that window. We stopped
  monitoring at that point rather than confirm a longer bound.

Neither attempt saw a recovery, so we cannot report a precise effective
timeout — only a firm lower bound of ~9.2 minutes where the code-inspected
default of ~120ms predicts recovery in a small fraction of a second. This
is either a *much* longer effective deadline than intended (large-scale
unit mismatch) or the hardware reset action isn't firing at all in this
configuration; we did not have time to distinguish the two (see "Ask"
below).

This directional result (much longer than intended, not shorter) is
consistent with the missing-conversion theory if the real per-tick duration
on this SoC/prescaler combination is longer than 1ms (i.e. the un-converted
raw ms value ends up programmed as a much smaller tick count than intended,
which — depending on the true tick period — could translate to either a
much shorter or much longer real deadline; empirically we observed
"longer").

## Suggested fix

Convert `data->timeout` (milliseconds) to hardware ticks using
`MWDT_TICK_PRESCALER`/`MWDT_TICKS_PER_US` (or whatever the correct derived
factor is for this SoC family) before calling `wdt_hal_config_stage()`,
following the same pattern `task_wdt_impl_timergroup.c` already uses in the
same tree. Given stage0/stage1 are configured with the *same* `data->timeout`
in the current code (no stage1-relative-to-stage0 doubling, unlike the
ESP-IDF reference's `2 * 1000 / TWDT_TICKS_PER_US` for stage1), it may also
be worth confirming whether `wdt_hal_config_stage()`'s stage timeouts are
absolute (both fire at the same deadline) or cascaded (stage1 fires stage1's
value *after* stage0 fires) — the correct fix depends on which.

## Ask

1. Can anyone confirm/deny the missing conversion from a fuller
   understanding of `wdt_hal_config_stage()`'s tick semantics for this SoC
   family than we have from black-box code reading?
2. If confirmed, is this specific to ESP32-C6, or does every
   `CONFIG_WDT_ESP32`-based board share this driver code path (a grep
   suggests yes — `wdt_esp32.c` is shared across the ESP32 family via
   `DT_DRV_COMPAT espressif_esp32_watchdog`)?
