# Performance handoff — fork changes vs upstream v0.5.0

> **What this is.** The `perf/connect-latency-dashboard` branch
> (`MandicReally/Prusa-Connect-Touch`), based on upstream `main` @ `e9e1efd`
> (v0.5.0 + network log pipe). It contains a focused set of performance changes,
> each verified on K-Touch hardware (landscape, real Prusa Connect fleet).
> This file describes the **final state only** — what changed, where, and why —
> so the changes can be compared against upstream and integrated selectively.

## Measured result (on hardware)

| Metric | Upstream v0.5.0 | This branch |
|---|---|---|
| Fleet dashboard scroll (fast fling) | 8 FPS, 110–130 ms/frame | **15 FPS, ~60 ms/frame** |
| Mid-scroll hitches from poll updates | yes | none (deferred to scroll-end) |
| Connect command POST (device-side) | waits out the whole poll cycle | sent at next checkpoint, seconds |
| Prusa Connect login / cloud fleet | works | works (re-verified on final build) |

Numbers from LVGL's perf overlay (`LV_USE_PERF_MONITOR`, enabled temporarily
during testing; off in the shipped config).

---

## The changes

### 1. Snapshot-cached dashboard cards — `firmware/main/ui.c` (the big one)

Scrolling was render-bound: every frame software-rendered each card's
rounded-corner clip masks, ~10 text labels, and PNG-decoded job thumbnail.
Now each card's live widget tree lives on a **hidden host screen**
(`s_card_host`, never loaded) and is rendered **once per data change** into a
per-slot 380×170 RGB565 `lv_draw_buf` (PSRAM, lazily allocated, reused across
rebuilds). The visible grid holds plain `lv_image` widgets showing those
bitmaps, so a scroll frame is a few opaque blits.

Properties worth knowing when integrating:
- **Pixel-identical at rest**: the card renders inside a `PP_BG` wrapper, so
  its rounded corners bake against the same color as the grid background.
  Offline dimming (`LV_OPA_70`) bakes the same way.
- **Clicks preserved**: the grid images carry `LV_OBJ_FLAG_CLICKABLE` + the
  same `on_card_clicked` callback and store-index user_data as before.
- **Re-snapshot only on visible change**: `update_dash_card()` now
  compares-before-set (`lbl_set_if_changed`) and returns whether anything
  visible changed; a publish that changes nothing re-renders nothing.
- **Graceful fallback**: if a slot's draw buf can't allocate, that slot builds
  a live card directly in the grid — exactly the old behavior, per slot.
- **Thumbnails**: PNG decode now happens at snapshot time, off the scroll path.
  Thumbnail arrival still flows through the existing structural-signature
  rebuild (`dash_sig`), unchanged.
- Memory: 16 slots × ~129 KB = **~2.1 MB PSRAM worst case**, lazy.
- Requires `CONFIG_LV_USE_SNAPSHOT=y` (added to `sdkconfig.defaults`; LVGL
  defaults it off).

### 2. Publish deferral during scroll — `firmware/main/ui.c`

A fleet publish landing mid-gesture used to update labels and invalidate cards,
stealing frames from an already slow scroll. `ui_apply_dashboard` now parks the
newest publish while the grid is scrolling (`LV_EVENT_SCROLL_BEGIN/END` on
`s_dash_grid`, including the momentum glide) and applies it at scroll-end.
Guarded with `lv_obj_is_scrolling()` so a lost SCROLL_END (e.g. screen switch
mid-throw) can't park publishes forever.

### 3. Pixel clock 23 → 17 MHz — `firmware/components/PandaTouch_IDF/include/pandatouch_board.h`

The frame buffer lives in PSRAM, so panel scan-out competes with rendering for
the same PSRAM bus *continuously*. Dropping the pixel clock (~54 Hz → ~40 Hz
panel refresh, invisible at these content frame rates) cuts that always-on read
traffic and the bounce-buffer copy tax by ~25%. No shimmer/flicker observed on
the tested K-Touch panel; revert to 23 MHz if a unit shows artifacts.

### 4. Single LVGL draw buffer — `firmware/sdkconfig.defaults`

`CONFIG_PT_LVGL_RENDER_PARTIAL_1=y` (was PARTIAL_2 ping-pong). The BSP's flush
is synchronous — `pt_lvgl_flush_cb` memcpys into the framebuffer and calls
`lv_display_flush_ready` immediately — so the second buffer never overlapped
render with flush; it was pure RAM waste. Freeing it costs nothing and the
reclaimed RAM is margin for TLS (see warning below).

### 5. Prompt Connect command servicing — `firmware/main/app_state.c`, `firmware/main/prusa_connect.c`

- `drain_commands()` runs queued UI commands (home/jog/preheat/pause/…) at
  three checkpoints inside the net task's poll loop (top, after the long fleet
  GET, before the per-printer poll) instead of only at the bottom — a button
  press now waits for at most one in-flight HTTP call, not the whole cycle.
  Same task, no new locking.
- Control-command POSTs (`connect_send_kwargs` / `connect_sync_cmd`) use an
  **8 s timeout** vs 15 s for bulk polls (`do_http_to`), so one stalled request
  can't hold the serialized-TLS mutex long.
- `net_task` is pinned to **core 0** so cloud TLS/crypto never contends with
  the LVGL render task on core 1.
- Local-poll dashboard publishes are coalesced to ≥2.5 s (`DASH_PUBLISH_MIN_MS`);
  cloud refreshes still publish immediately. The detail screen is unaffected.

### 6. LVGL cadence — `firmware/sdkconfig.defaults`

`CONFIG_LV_DEF_REFR_PERIOD=16` and `CONFIG_LV_INDEV_DEF_READ_PERIOD=16`
(default ~33 ms): better touch-follow while scrolling; render-bound screens are
unaffected (they're CPU-limited anyway).

---

## ⚠ Critical warning — do not raise the ESP32-S3 cache sizes

`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` / `CONFIG_ESP32S3_DATA_CACHE_64KB`
gain ~2 FPS — and **break Prusa Connect**. The 48 KB they carve out of internal
SRAM starves mbedTLS (which must allocate internal RAM): login returns
"Login Failed" and cloud printers stop reporting, while plain-HTTP PrusaLink
keeps working. **Hardware-verified twice** (with two draw buffers and with
one). Compensating by freeing a draw buffer did not work — note that
`pt_malloc_caps` falls back to PSRAM *silently*, so an "internal" buffer you
free may already have been living in PSRAM. A warning comment marks this in
`sdkconfig.defaults`. If anyone revisits internal-RAM tuning: watch
`/api/info` → `heap_internal` / `heap_internal_min` before and after.

## Fork-only artifacts (adjust on integration)

- **`PP_FW_VERSION` is set to a fork test version (`0.4.9.x`)** in
  `firmware/main/pandaprusa.h` purely so test devices report which image they
  run. Restore upstream's versioning on integration. (Side effect while it
  stands: the GitHub update check offers upstream v0.5.0 as an "update".)
- `CONFIG_LV_USE_PERF_MONITOR` is **off** in the final config; flip to `=y` to
  reproduce the measurements.
- This file (`PERF-HANDOFF.md`) — drop after integration.

## Verification suggestions

1. Build, flash, confirm the dashboard looks pixel-identical at rest
   (corners, offline dimming, thumbnails, temps updating in place).
2. Tap cards → detail screen opens as before.
3. Enable `LV_USE_PERF_MONITOR=y`, fast top-to-bottom fling on a multi-printer
   fleet: expect ~15 FPS / ~60 ms (vs ~8 / ~120 upstream), no mid-scroll hitches.
4. **Prusa Connect**: log in, confirm live cloud statuses, send HOME from the
   control screen — the POST should appear in the log (`cmd HOME -> 201`)
   within a few seconds of the press.

Further headroom exists (e.g. S3 SIMD blend routines, direct-framebuffer
scan-out) but those are bench-only experiments with real risk — additional
notes on what was tried and rejected are available on request.
