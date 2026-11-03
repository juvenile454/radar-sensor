# Agent instructions

Everything about the radar project lives in this folder (incl. the Android app under `android/`).
Status and phases: `docs/CONCEPT.md`. Pins/wiring: `docs/HARDWARE.md`.
Measurement plan/results: `docs/EXPERIMENTS.md`. App plan: `docs/ANDROID_APP.md`. Vital audit and plan: `docs/VITAL_AUDIT.md`.
Pico and SAR scanner are on hold.

## Rules

- Do not put recordings, datasets, firmware binaries or vendor downloads into the repo;
  keep data in `~/Data/radar-sensor/`.
- Do not check in the Acconeer SDK/Waveshare demos (license); only note the download source and version.
- Before flashing/uploading, identify the device and the port unambiguously (`ls -l /dev/serial/by-id`).
- Phone: install only with approval from the project owner; always take the serial number live from `adb devices -l`.
- Operate radio modules unmodified. No claims about pulse accuracy without a reference measurement.
- Build/compile is not a runtime check on the device; report the two separately.
