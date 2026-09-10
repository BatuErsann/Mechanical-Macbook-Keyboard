# Haptik

Haptik is a native macOS app that plays mechanical keyboard sounds while you type.
It estimates typing impact from a compatible MacBook’s built-in accelerometer and
uses that estimate to vary sound intensity. It does **not** measure actual key
pressure.

The app includes its own icon and sound assets. Once built, double-click
**Haptik.app** in Finder to launch it; no Terminal is needed to run it. The app
interface follows the preferred language in macOS. It includes English, Turkish,
Spanish, French, German, Italian, Portuguese, Japanese, Korean, and Simplified
Chinese. Unsupported languages fall back to English.

## Features

- Accelerometer input through Apple’s SPU HID interface, requesting a 1 kHz report rate.
- Real-time gravity and background-noise filtering for impact estimation.
- Global keyboard capture and a low-latency Core Audio playback engine.
- Fourteen recorded switch sound packs, with dedicated Space, Enter, and Backspace
  samples where provided by the pack.
- Menu bar controls for sound selection, volume, sensitivity, and enabling playback.
- A status window showing sensor activity, detected key count, and current impact.
- Automatic retries for sensor startup failures, stalled streams, and wake from sleep.

## Requirements

- An Apple Silicon MacBook with a compatible built-in SPU accelerometer.
  Sensor availability is checked at runtime; support is not guaranteed for every model.
- macOS 13 or later.
- Xcode or Xcode Command Line Tools to build from source.
- Input Monitoring permission; the app also requests Accessibility permission.

The sensor integration uses an undocumented Apple interface. Compatibility may vary
with hardware and macOS versions.

## Build and launch

Run from the repository root:

```sh
make app
open build/Haptik.app
```

Alternatively, double-click `build/Haptik.app` in Finder. Drag it to Applications
if you want to keep it there. All runtime assets are inside the bundle.

The build uses a local ad hoc signature. Developer ID signing, notarization, and a
DMG installer are not configured.

## First launch

1. Open Haptik and allow the requested permissions in **System Settings → Privacy
   & Security → Input Monitoring** and **Accessibility**.
2. Use **Input Monitoring** in the app to open the relevant system setting, or
   **Check permissions** to retry permission checks.
3. If macOS asks you to restart the app after granting permission, quit Haptik from
   its menu bar menu and launch it again.
4. Type on the built-in keyboard and check the detected key count and sensor status.
   Use **Test sound** to test audio playback independently of keyboard input.

Closing the window keeps Haptik running in the menu bar. Choose **Open Haptik**
from the menu, or open the app again, to return to the status window. Choose
**Quit Haptik** to quit.

## Troubleshooting

- **No keys detected:** check Input Monitoring and Accessibility permissions for
  the copy of Haptik you are running, then restart the app if needed.
- **No sensor data:** check Input Monitoring permission and the sensor status in
  the window. Haptik retries stalled streams automatically. A detected device alone
  does not confirm that samples are arriving.
- **No sound:** enable playback in the menu bar, raise the app volume, check the
  system output device, and use the sound test button.
- **Sound does not vary with typing:** confirm the sensor is receiving samples and
  adjust sensitivity. Chassis vibration is only an estimate of typing impact;
  external keyboards do not provide per-key force data through this sensor.

The app writes startup and initial input diagnostics to `/private/tmp/haptik-debug.log`.
The log is replaced on each launch.

## Tests and sensor diagnostics

Run from the repository root so the audio tests can find the source sound assets:

```sh
make test
make probe
./build/haptik-sensor-probe
```

The tests cover impact filtering and audio pack loading. The standalone probe
reports sensor availability, access status, sample rate, and acceleration peaks.
When running it from Terminal, Terminal may need its own Input Monitoring permission.
Automated tests do not replace checking physical keyboard input and sensor access
in the running app.

## Project structure

```text
haptik/
├── Makefile                    # Build, bundle, signing, and test targets
├── README.md
├── THIRD_PARTY_NOTICES.md       # Dependency and sound attribution
├── include/                    # Shared C interfaces
│   ├── haptik_audio.h
│   ├── haptik_impact.h
│   └── haptik_sensor.h
├── src/
│   ├── core/
│   │   ├── haptik_audio.c       # Audio playback and sound pack decoding
│   │   └── haptik_impact.c      # Impact filtering and intensity estimation
│   └── macos/
│       ├── main.m              # App lifecycle, UI, permissions, keyboard capture
│       └── haptik_sensor.c     # IOKit sensor access and sampling thread
├── resources/
│   ├── macos/Info.plist        # Application bundle metadata
│   └── sounds/                 # Sound packs and their license/source files
├── tools/
│   ├── make_icon.m             # Native app icon generator
│   └── sensor_probe.c          # Standalone sensor diagnostic tool
├── tests/                      # Impact and audio pack tests
├── third_party/minimp3/        # Vendored MP3 decoder and license
└── build/                      # Generated app, objects, tools, and test binaries
```

Generated files stay in `build/`, which is ignored by Git. `make clean` removes
that directory.

## How it works

```text
SPU accelerometer → sensor thread → gravity/noise filter → published intensity
                                                                ↓
Global KeyDown ───────────────────────────────────────→ intensity lookup
                                                                ↓
                                                audio event queue → Core Audio
```

The C modules handle sampling, impact estimation, and audio. The Objective-C app
layer manages the window, menu bar, permissions, and keyboard events. If no recent
sensor sample is available, keyboard playback uses a fallback intensity.

## Attribution

Sensor report parsing draws on `olvvier/apple-silicon-accelerometer`. The MP3 decoder and bundled recordings have
their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the
license/source files alongside the sound packs.
