"""ARCHIV (V5.3, docs/AUDIT_VITAL.md), nicht mehr gepflegt.
Live-Anzeige mit der DSP-Kette aus a121_vitalaufnahme.py. Neue Auswertungen nur mit host/vital_eval.py und Varianten
des Firmware-Headers firmware/esp32s3/src/a121_esp_dsp.h (--replay); was sich hier bewährt, wandert in den Header.
Funktion des Skripts unverändert.

Live-Monitor A121: Abstand, Bewegung, Atmung, Puls-Kandidat — zum Beobachten, nicht zum Messen.

Fenster (pyqtgraph) mit:
- Amplitude über Abstand (stabilster Ziel-Bin im Brustecho markiert)
- Brustverschiebung der letzten 30 s (kreiszentrierte DACM-Phase am Ziel, mm)
- Pulsband 0,8–2,5 Hz nach Hilbert-Atmungsoberwellen-Projektion (µm)
- Spektrum mit Atmungs-Oberwellen + Beschleunigungs-HPS (f0·2f0·3f0)
- Kennzahlen: Abstand, Signal, Bewegung, Atmung /min, Puls-Kandidat /min (Spektrum & HPS)
Button „Letzte 60 s speichern“ → ~/Data/radar-sensor/.

  sg dialout -c ".venv/bin/python host/a121_livemonitor.py <PORT> [START_M END_M]"

Puls-Anzeige ist experimentell und ohne Referenz keine Messung.
"""
import json
import queue
import sys
import threading
import time
from pathlib import Path

import numpy as np
import pyqtgraph as pg
from acconeer.exptool import a121
from PySide6 import QtCore, QtWidgets
from scipy import signal

from a121_vitalaufnahme import (
    dacm_displacement_mm,
    hps_cardiac_peaks,
    project_out_respiration_harmonics,
)

START_POINT, NUM_POINTS = 60, 320  # Standard 0,15–0,95 m (deckt 150 mm bis 950 mm ab), 2,5 mm/Punkt
POINT_M = 0.0025
FS = 50.0
MM_PER_RAD = 5.0 / (4 * np.pi)
BUF_S = 60
VIEW_S = 30
DATA_DIR = Path("~/Data/radar-sensor").expanduser()


class Reader(threading.Thread):
    def __init__(self, port: str, cfg: a121.SensorConfig, out: queue.Queue):
        super().__init__(daemon=True)
        self.port, self.cfg, self.out = port, cfg, out
        self.stop_evt = threading.Event()
        self.status = "verbinde …"

    def run(self) -> None:
        client = None
        for i in range(3):  # override_baudrate=921600 vermeidet den 2M-Baud-Reconnect-Timeout
            try:
                client = a121.Client.open(serial_port=self.port, override_baudrate=921600)
                client.setup_session(self.cfg)
                break
            except Exception as e:  # noqa: BLE001
                self.status = f"Verbindung {i + 1} fehlgeschlagen: {e}"
                if client is not None:
                    try:
                        client.close()
                    except Exception:  # noqa: BLE001
                        pass
                client = None
                time.sleep(0.5)
        if client is None:
            self.status = "keine Verbindung"
            return
        client.start_session()
        self.status = "läuft"
        try:
            while not self.stop_evt.is_set():
                self.out.put(client.get_next().frame[0])
        except Exception as e:  # noqa: BLE001
            self.status = f"Fehler: {e}"
        finally:
            try:
                client.stop_session()
                client.close()
            except Exception:  # noqa: BLE001
                pass


def peak_bpm(x, fs, fmin, fmax, nseg_s=20):
    if len(x) < fs * 8:
        return None, None, None
    f, p = signal.welch(x, fs=fs, nperseg=min(len(x), int(fs * nseg_s)), nfft=int(fs * 120))
    m = (f >= fmin) & (f <= fmax)
    if not m.any() or p[m].max() <= 0:
        return None, f, p
    return float(f[m][np.argmax(p[m])] * 60), f, p


class Monitor(QtWidgets.QWidget):
    def __init__(self, port: str, start_point: int = START_POINT, num_points: int = NUM_POINTS):
        super().__init__()
        self.setWindowTitle("A121 Live-Monitor (PROFILE_1 + DACM + Atmungs-Projektion)")
        self.cfg = a121.SensorConfig(
            start_point=start_point,
            num_points=num_points,
            step_length=1,
            profile=a121.Profile.PROFILE_1,
            hwaas=32,
            sweeps_per_frame=1,
            frame_rate=FS,
            phase_enhancement=True,
            iq_imbalance_compensation=True,
            prf=a121.PRF.PRF_19_5_MHz,
            inter_frame_idle_state=a121.IdleState.READY,
            inter_sweep_idle_state=a121.IdleState.READY,
        )
        self.q: queue.Queue = queue.Queue()
        self.reader = Reader(port, self.cfg, self.q)
        self.buf = np.zeros((int(BUF_S * FS), num_points), dtype=np.complex64)
        self.n = 0  # Frames insgesamt
        self.dist = (start_point + np.arange(num_points)) * POINT_M
        self.target = None
        self.sos_b = signal.butter(2, [0.1, 0.6], btype="band", fs=FS, output="sos")
        self.sos_h = signal.butter(4, [0.8, 2.5], btype="band", fs=FS, output="sos")

        pg.setConfigOptions(antialias=True)
        lay = QtWidgets.QHBoxLayout(self)
        left = QtWidgets.QVBoxLayout()
        self.lbl = {}
        for key in ["Status", "Abstand", "Signal", "Bewegung", "Atmung", "Puls-Kandidat"]:
            l = QtWidgets.QLabel(f"{key}: –")
            l.setStyleSheet("font-size: 20px; padding: 4px;")
            left.addWidget(l)
            self.lbl[key] = l
        self.lbl["Hinweis"] = QtWidgets.QLabel("")
        self.lbl["Hinweis"].setWordWrap(True)
        self.lbl["Hinweis"].setStyleSheet("font-size: 14px; color: #c80;")
        left.addWidget(self.lbl["Hinweis"])
        btn = QtWidgets.QPushButton("Letzte 60 s speichern")
        btn.clicked.connect(self.save)
        left.addWidget(btn)
        left.addStretch(1)
        lw = QtWidgets.QWidget(); lw.setLayout(left); lw.setFixedWidth(350)
        lay.addWidget(lw)

        glw = pg.GraphicsLayoutWidget()
        lay.addWidget(glw, 1)
        p0 = glw.addPlot(title="Amplitude über Abstand (PROFILE_1)"); p0.setLabel("bottom", "m")
        self.c_amp = p0.plot(pen="y"); self.l_tgt = pg.InfiniteLine(angle=90, pen="r"); p0.addItem(self.l_tgt)
        glw.nextRow()
        p1 = glw.addPlot(title="Brustverschiebung DACM (mm), letzte 30 s"); p1.setLabel("bottom", "s")
        self.c_disp = p1.plot(pen=pg.mkPen("c", width=1)); self.c_br = p1.plot(pen=pg.mkPen("m", width=2))
        glw.nextRow()
        p2 = glw.addPlot(title="Pulsband 0,8–2,5 Hz nach Oberwellen-Projektion (µm)"); p2.setLabel("bottom", "s")
        self.c_heart = p2.plot(pen="g")
        glw.nextRow()
        p3 = glw.addPlot(title="Spektrum (/min) — grau: Atmungs-Oberwellen"); p3.setLogMode(y=True)
        p3.setXRange(0, 160); p3.setLabel("bottom", "/min")
        self.c_spec = p3.plot(pen="c"); self.c_spec_h = p3.plot(pen="g")
        self.harm_lines = [pg.InfiniteLine(angle=90, pen=pg.mkPen((150, 150, 150), style=QtCore.Qt.DotLine))
                           for _ in range(8)]
        for hl in self.harm_lines:
            p3.addItem(hl)

        self.timer = QtCore.QTimer(self); self.timer.timeout.connect(self.update_view); self.timer.start(200)
        self.reader.start()
        self.resize(1400, 900)

    def window(self, seconds):
        m = min(self.n, len(self.buf), int(seconds * FS))
        idx = (np.arange(self.n - m, self.n)) % len(self.buf)
        return self.buf[idx]

    def update_view(self):
        while True:
            try:
                fr = self.q.get_nowait()
            except queue.Empty:
                break
            self.buf[self.n % len(self.buf)] = fr
            self.n += 1
        self.lbl["Status"].setText(f"Status: {self.reader.status}")
        if self.n < FS * 2:
            return

        w = self.window(VIEW_S)
        amp_mean = np.abs(w).mean(axis=0)
        amp_min = np.min(np.abs(w), axis=0)
        amp_std = np.abs(w).std(axis=0) + 1e-6
        k_raw = int(np.argmax(amp_mean))
        mask = amp_mean >= 0.30 * amp_mean[k_raw]
        qual = np.where(mask, (amp_min / amp_std) * (amp_min / amp_mean[k_raw]), -1.0)
        k_new = int(np.argmax(qual))
        # Ziel nur wechseln, wenn die Phasenstabilität am neuen Bin klar besser ist
        if self.target is None or qual[k_new] > 1.35 * max(qual[self.target], 1e-6):
            self.target = k_new
        k = self.target
        noise = float(np.median(amp_mean))
        self.c_amp.setData(self.dist, amp_mean)
        self.l_tgt.setValue(self.dist[k])
        self.lbl["Abstand"].setText(f"Abstand: {self.dist[k] * 1000:.0f} mm (Bin {k})")
        self.lbl["Signal"].setText(f"Signal: {20 * np.log10(amp_mean[k] / max(noise, 1)):.1f} dB über Median")

        sl = slice(max(k - 2, 0), min(k + 3, w.shape[1]))
        sub = w[:, sl]
        ref = w[:, k : k + 1]
        phase_align = np.exp(-1j * np.angle((sub * np.conj(ref)).mean(axis=0, keepdims=True)))
        z = (sub * phase_align).mean(axis=1)
        disp = dacm_displacement_mm(z)
        t = (np.arange(len(disp)) - len(disp)) / FS
        self.c_disp.setData(t, disp)

        # Bewegung: Spitze-Spitze der letzten 2 s nach Abzug des linearen Trends
        last = signal.detrend(disp[-int(2 * FS):])
        mv = float(np.ptp(last))
        state = "ruhig" if mv < 1.5 else ("leicht" if mv < 5 else "STARK")
        self.lbl["Bewegung"].setText(f"Bewegung: {state} ({mv:.1f} mm/2 s)")

        if len(disp) < FS * 10:
            self.lbl["Hinweis"].setText("sammle Daten (mind. 10 s) …")
            return
        dd = signal.detrend(disp)
        br_sig, disp_clean = project_out_respiration_harmonics(dd, fs=FS, max_harm=5)
        heart = signal.sosfiltfilt(self.sos_h, disp_clean)
        self.c_br.setData(t, br_sig)
        self.c_heart.setData(t, heart * 1000)

        br, f, p = peak_bpm(dd, FS, 0.1, 0.6)
        hr, fh, ph = peak_bpm(heart, FS, 0.8, 2.5)
        _, _, hps_pks = hps_cardiac_peaks(disp_clean, fs=FS, fmin=0.85, fmax=2.1, n=1)
        hr_hps = hps_pks[0][0] if hps_pks else None
        if f is not None and fh is not None and p is not None and ph is not None:
            m = f * 60 < 170
            self.c_spec.setData(f[m] * 60, p[m] + 1e-12)
            self.c_spec_h.setData(fh[m] * 60, ph[m] + 1e-12)
        self.lbl["Atmung"].setText(f"Atmung: {br:.1f} /min" if br else "Atmung: –")
        hint = []
        for i, hl in enumerate(self.harm_lines):
            hl.setValue(br * (i + 1) if br else -10)
        if hr:
            hps_str = f" (HPS: {hr_hps:.0f})" if hr_hps else ""
            txt = f"Puls-Kandidat: {hr:.0f}{hps_str} /min"
            if br and any(abs(hr - br * h) < 2.5 for h in range(2, 9)):
                txt += " ⚠"
                hint.append("Spektral-Peak nahe Atmungs-Oberwelle — mit HPS abgleichen.")
            self.lbl["Puls-Kandidat"].setText(txt)
        if mv >= 1.5:
            hint.append("Bewegung verfälscht Atmung/Puls.")
        hint.append("Experimentell, ohne Referenz keine Messung.")
        self.lbl["Hinweis"].setText("\n".join(hint))

    def save(self):
        w = self.window(BUF_S)
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        out = DATA_DIR / time.strftime("%Y-%m-%d_%H%M%S_live.npz")
        np.savez_compressed(out, iq=w, distances_m=self.dist, fs=FS, target_bin=-1 if self.target is None else self.target,
                            meta=json.dumps({"setup": "Live-Monitor-Mitschnitt",
                                             "sensor_config": self.cfg.to_json()}))
        self.lbl["Hinweis"].setText(f"gespeichert: {out.name}")

    def closeEvent(self, e):
        self.reader.stop_evt.set()
        self.reader.join(timeout=3)
        super().closeEvent(e)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    sp, np_pts = START_POINT, NUM_POINTS
    if len(sys.argv) == 4:
        s_m, e_m = float(sys.argv[2]), float(sys.argv[3])
        sp = int(round(s_m / POINT_M))
        np_pts = int(round((e_m - s_m) / POINT_M))
    app = QtWidgets.QApplication(sys.argv[:1])
    w = Monitor(sys.argv[1], start_point=sp, num_points=np_pts)
    w.show()
    sys.exit(app.exec())
