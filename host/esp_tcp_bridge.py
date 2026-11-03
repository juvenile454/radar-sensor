#!/usr/bin/env python3
"""Development relay ESP32 ↔ TCP (tailnet only) for the Android debug app.

The ESP32 stays at the PC (USB), the app connects via TCP over Tailscale and sees the same byte stream as over
USB-OTG. For development only (phase A2), not as a sensor connection.

  sg dialout -c ".venv/bin/python host/esp_tcp_bridge.py"            # listens on <Tailscale-IPv4>:7777
  sg dialout -c ".venv/bin/python host/esp_tcp_bridge.py --log x.log" # additionally write everything received to a log

- Listens only on the Tailscale address (otherwise explicitly with --bind), one client at a time; a new one replaces the old one.
- Keeps reading the ESP32 even without a client: if nobody reads, any output slows down its DSP loop.
- If the client disconnects during a stream, the stream is ended with a line ending (any character stops
  iq3/doppler/sess run) so the ESP32 does not keep streaming forever.
- DTR/RTS are not touched (RTS with DTR=0 resets the ESP32-S3 via USB-Serial/JTAG).
"""

from __future__ import annotations

import argparse
import pathlib
import socket
import subprocess
import sys
import threading
import time

BY_ID = pathlib.Path("/dev/serial/by-id")


def find_port() -> str:
    ports = sorted(p for p in BY_ID.glob("usb-Espressif*") if p.name.endswith("-if00"))
    if len(ports) != 1:
        raise SystemExit(f"FEHLER: genau ein ESP32 unter {BY_ID} erwartet, gefunden: {[p.name for p in ports]}")
    return str(ports[0])


def tailscale_ip() -> str:
    try:
        out = subprocess.run(["tailscale", "ip", "-4"], capture_output=True, text=True, timeout=5, check=True).stdout
    except (OSError, subprocess.SubprocessError) as e:
        raise SystemExit(f"FEHLER: Tailscale-Adresse nicht lesbar ({e}); mit --bind angeben") from e
    ip = out.split()[0] if out.split() else ""
    if not ip.startswith("100."):
        raise SystemExit(f"FEHLER: unerwartete Tailscale-Adresse '{ip}'; mit --bind angeben")
    return ip


class Bridge:
    def __init__(self, ser, log_file):
        self.ser = ser
        self.log = log_file
        self.lock = threading.Lock()
        self.client: socket.socket | None = None
        self.last_rx = 0.0
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.dropped = 0
        self.running = True

    def serial_reader(self) -> None:
        while self.running:
            data = self.ser.read(4096)
            if not data:
                continue
            self.last_rx = time.monotonic()
            self.rx_bytes += len(data)
            if self.log:
                self.log.write(data)
            with self.lock:
                c = self.client
            if c is None:
                self.dropped += len(data)
                continue
            try:
                c.sendall(data)
            except OSError:
                self.drop_client(c, "Senden fehlgeschlagen")

    def client_reader(self, c: socket.socket, addr) -> None:
        try:
            while True:
                data = c.recv(4096)
                if not data:
                    break
                self.tx_bytes += len(data)
                self.ser.write(data)
        except OSError:
            pass
        self.drop_client(c, "getrennt")

    def drop_client(self, c: socket.socket, why: str) -> None:
        with self.lock:
            if self.client is not c:
                return
            self.client = None
        try:
            c.close()
        except OSError:
            pass
        streaming = time.monotonic() - self.last_rx < 0.5
        print(f"[bridge] Client {why}" + ("; Stream wird beendet" if streaming else ""), flush=True)
        if streaming:
            self.ser.write(b"\n")

    def set_client(self, c: socket.socket, addr) -> None:
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c.settimeout(None)
        with self.lock:
            old, self.client = self.client, c
        if old is not None:
            print("[bridge] neuer Client ersetzt den alten", flush=True)
            try:
                old.close()
            except OSError:
                pass
        print(f"[bridge] Client verbunden: {addr[0]}:{addr[1]}", flush=True)
        threading.Thread(target=self.client_reader, args=(c, addr), daemon=True).start()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serieller Port (Standard: einziger ESP32 unter /dev/serial/by-id)")
    ap.add_argument("--bind", help="Adresse (Standard: eigene Tailscale-IPv4)")
    ap.add_argument("--tcp-port", type=int, default=7777)
    ap.add_argument("--log", type=pathlib.Path, help="alles vom ESP32 Empfangene zusätzlich in diese Datei")
    args = ap.parse_args()

    import serial  # pyserial

    port = args.port or find_port()
    bind = args.bind or tailscale_ip()
    ser = serial.Serial(port, 115200, timeout=0.05, exclusive=True)
    log = open(args.log, "ab") if args.log else None  # noqa: SIM115
    br = Bridge(ser, log)
    threading.Thread(target=br.serial_reader, daemon=True).start()

    srv = socket.create_server((bind, args.tcp_port), reuse_port=False)
    srv.settimeout(1.0)
    print(f"[bridge] {port} ↔ tcp://{bind}:{args.tcp_port} (Strg+C beendet)", flush=True)
    t_stat = time.monotonic()
    rx0 = 0
    try:
        while True:
            try:
                c, addr = srv.accept()
                br.set_client(c, addr)
            except TimeoutError:
                pass
            now = time.monotonic()
            if now - t_stat >= 30:
                rate = (br.rx_bytes - rx0) / (now - t_stat)
                print(f"[bridge] ESP→ {rate / 1000:.1f} kB/s, gesamt {br.rx_bytes} B, an App {br.tx_bytes} B zurück, "
                      f"ohne Client verworfen {br.dropped} B", flush=True)
                t_stat, rx0 = now, br.rx_bytes
    except KeyboardInterrupt:
        pass
    finally:
        br.running = False
        with br.lock:
            c = br.client
        if c is not None:
            br.drop_client(c, "Bridge beendet")
        srv.close()
        time.sleep(0.1)
        ser.close()
        if log:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
