# Agent instructions

Alles zum Radarprojekt liegt in diesem Ordner (inkl. Android-App unter `android/`).
Stand und Phasen: `docs/KONZEPT.md`. Pins/Verdrahtung: `docs/HARDWARE.md`.

## Regeln

- Aufnahmen, Datensätze, Firmware-Binärdateien und Hersteller-Downloads nicht ins Repo;
  Daten nach `~/Data/radar-sensor/`.
- Acconeer-SDK/Waveshare-Demos nicht einchecken (Lizenz); nur Download-Quelle und Version notieren.
- Vor Flashen/Upload das Gerät und den Port eindeutig identifizieren (`ls -l /dev/serial/by-id`).
- Handy: nur mit Freigabe des Projektinhabers installieren; Seriennummer immer live aus `adb devices -l`.
- Funkmodule unverändert betreiben. Keine Aussagen über Puls-Genauigkeit ohne Referenzmessung.
- Build/Compile ist keine Laufzeitprüfung am Gerät; beides getrennt ausweisen.
