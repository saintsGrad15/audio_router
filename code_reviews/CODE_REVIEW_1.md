# Code Review: audio_router.c

## Memory Leaks

### 1. `RenderContext` + nested buffers never freed (lines 661, 711, 836)

All three paths in `setup_audio()` allocate a `RenderContext` via `calloc()`, and the non-test-mode paths also allocate `ctx->input_buf` with nested `calloc()` calls for each buffer's `mData`. The cleanup in `main()` (lines 1034-1048) disposes the AudioUnits but **never frees the RenderContext or its buffers**. This leaks on every exit (including normal Ctrl+C shutdown).

### 2. `g_meter_input_buf` + nested buffers never freed (lines 527-533)

`setup_meter()` allocates `g_meter_input_buf` and its per-buffer `mData` via `calloc()`. The meter-mode cleanup (lines 979-988) stops and disposes the AudioUnits but **never frees `g_meter_input_buf` or any of its nested `mData` pointers**. This is a multi-allocation leak: 1 outer struct + N inner buffers.

### 3. Test tone mode has no cleanup at all

When `test_mode` is true in `main()`, execution flows to the idle loop (line 1029) and cleanup block (lines 1034-1048). But `g_au` is NULL in test mode (only `g_output_au` is set), so the existing cleanup correctly handles `g_output_au`. However, the `RenderContext` allocated at line 661 is still leaked, as noted above. There is no mode-specific cleanup difference — the leak exists in all three code paths.

---

## Performance Issues

### 4. `render_callback` hardcodes sample rate to 44100 Hz (line 318)

```c
double sr = 44100.0;
```

If the output device runs at 48000, 96000, or any other rate, the test tone will be pitch-shifted. This should query the actual device sample rate and pass it through `RenderContext`. This isn't just a correctness bug — the phase accumulator will drift relative to the real sample clock, potentially causing audible artifacts.

### 5. `sin()` called per-sample in the render callback (line 325)

```c
float sample = (float)(amplitude * sin(ctx->phase));
```

`sin()` is a transcendental function with no hardware fast-path on ARM64. For a real-time audio callback this is suboptimal. Alternatives:

- **Table lookup** (small fixed-size sine table + linear interpolation) — constant-time, cache-friendly
- **`sincos()`** if you ever need both sin and cos
- **Polynomial approximation** (e.g., a 3rd/5th-order minimax polynomial over one period) — very fast, <1 ULP error is acceptable for audio

At 440 Hz / 44100 Hz, this is called ~44100 times/sec. A sine table approach would be ~10x faster.

### 6. `display_meter()` busy-waits with `usleep(50000)` (line 974)

This burns CPU cycles doing nothing 20 times/sec. A `usleep()` or `nanosleep()` based approach is fine for a CLI tool, but if this ever needed to be more efficient, a condition variable with a timed wait would avoid the wake-sleep cycle entirely. Low priority for a CLI utility.

### 7. Device enumeration does redundant `malloc`/`free` per device (lines 83, 97)

`get_audio_devices()` calls `malloc` + `free` twice per device to query channel counts. The sizes are typically small (a few hundred bytes). You could:

- Use `alloca()` (stack-allocated, zero-cost)
- Use a fixed-size stack buffer (e.g., 1024 bytes) and fall back to malloc only if needed

This function is only called once or twice at startup, so the impact is negligible, but it's a minor code-quality improvement.

### 8. `volatile` instead of atomics for cross-thread peak values (lines 264-265, 361-362)

```c
volatile float peak_l;
volatile float peak_r;
```

`volatile` does not guarantee atomicity or memory ordering on multi-core ARM64. While a 32-bit float write is likely atomic on alignment, the compiler and CPU are free to reorder reads/writes. Using `_Atomic float` (or `stdatomic.h`) would be correct and costs nothing on ARM64 since `float` loads/stores are naturally atomic.

### 9. Signal handler calls non-async-signal-safe functions (lines 27-33)

```c
static void signal_handler(int sig) {
    ...
    if (g_output_au) AudioOutputUnitStop(g_output_au);
```

`AudioOutputUnitStop()` is not async-signal-safe. If the signal arrives while another AudioToolbox call is in progress, this could deadlock. The safer pattern is to just set `g_running = 0` in the handler and let the main loop handle the stop.

---

## Summary

| Priority | Issue | Impact |
|----------|-------|--------|
| **High** | `RenderContext` + buffers leaked (all modes) | Memory leak on every run |
| **High** | `g_meter_input_buf` leaked (meter mode) | Memory leak on every run |
| **High** | Signal handler calls non-signal-safe functions | Potential deadlock |
| **Medium** | Test tone hardcodes 44100 Hz | Wrong pitch on non-44100 devices |
| **Medium** | `sin()` per-sample in real-time callback | Unnecessary CPU cost |
| **Medium** | `volatile` instead of atomics | Technically undefined behavior |
| **Low** | Busy-wait in meter display loop | Minor CPU waste |
| **Low** | Redundant malloc/free in device enumeration | Negligible startup cost |
