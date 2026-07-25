# audio_router

Low-latency audio input-to-output routing utility for macOS, built on CoreAudio's AUHAL Audio Unit API.

## Building

```sh
make
```

Requires only the macOS SDK — no external dependencies.

## Usage

```
audio_router [options]

Options:
  -l              List audio devices
  -t              Play 440 Hz test tone (output only)
  -m              Meter input levels (input only)
  -i <device>     Input device (index or name substring)
  -o <device>     Output device (index or name substring)
  -b <frames>     Buffer size in frames (default: 256)
  -h              Show help
```

### Examples

```sh
./audio_router -l                  # List all audio devices
./audio_router -t                  # Play test tone on default output
./audio_router -m -i 7             # Meter input levels from device 7
./audio_router -i 7 -o 6           # Route device 7 input → device 6 output
./audio_router -b 128              # Smaller buffer (~2.7ms at 48kHz)
```

## How it works

The app uses Apple's CoreAudio framework, specifically a component called **AUHAL** (Audio Unit Hardware Abstraction Layer). An AUHAL is a bidirectional audio pipe — it can read from a device (input) and write to a device (output).

**Metering (`-m`)**: Two AUHALs are created — one connected to the input device and one to an output device. The output device exists only to keep the audio system "ticking" (it outputs silence). The input device's callback fires whenever new audio arrives, measures the peak level, and stores it in a shared variable. The main loop reads that variable 20 times per second and draws the bar.

**Routing (`-i` / `-o`)**: Same two-AUHAL setup, but the input callback buffers the audio samples, and the output render callback copies them to the output device instead of outputting silence.

**Same-device routing**: When input and output are the same device, a single AUHAL handles both sides.

## Known issue that was fixed

AUHAL has two sides: an input side (bus 1) and an output side (bus 0). The output side drives the system — when the output device needs audio, it "pulls" data by calling a render callback.

The original code tried to pull input data from inside the output render callback. This doesn't work because the input side needs its own push mechanism to actually start capturing. Without it, the input side sits idle producing zeros.

The fix was to register a separate **input callback** on the input AUHAL via `kAudioOutputUnitProperty_SetInputCallback`. This tells macOS to call the callback whenever new input audio arrives. Once registered, the input device starts actually capturing audio. The output render callback then reads from the buffer the input callback already filled.

In event-driven terms: the broken code was polling (`give me data now`), when it needed an event listener (`call me when data arrives`).
