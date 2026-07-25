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

**Note: In my experience on a 2024 MacBook Pro running Tahoe 26.5.2 with 36 GB of RAM, setting the audio buffer to `128` was necessary to avoid audible artifacts.**

## Intended use case

This utility was not designed and is likely unsuitable for recording or live audio. It was designed so that I could have all my audio coming out of one interface (in my case a Focusrite Scarlett 2i2). I then wanted to be able to connect a Boss GX-10 via USB to my laptop and route that audio directly to an output.

This avoids me having to consume the 2i2's inputs with the pedal outputs. It also avoids switching my monitors' (speakers) inputs manually (or using a hardware mixer of some sort) to alternate between the 2i2 signal and the GX-10 signal.

In the end, my specific pieces of hardware are irrelevant. This utility allows me to:
1. Connect two USB audio interfaces to my MacOS
2. Play the audio signal input from one directly through the other's output.
3. Enable the output interface (the one connected to speakers) to continue to be fed by **other** devices such as Logic Pro, Spotify, or Zoom.

**Note: If you are connecting an external interface for the purpose of recording into a DAW, this utility is not necessary. You can connect them both to the MacOS computer, select one as an input inteface and one as the output interface. A DAW is likely to be much more effective as processing audio streams.**

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

## Authorship

This code was written entirely by [OpenCode](https://opencode.ai/) and the "Big Pickle" model. OpenCode is an open source alternative to Claude Code and Big Pickle is a hosted, free-to-use model.

I'm a software engineer but I do not write C. I didn't personally review the code though I did clear context and ask Big Pickle to review the code and look for memory leaks and performance opportunities.
