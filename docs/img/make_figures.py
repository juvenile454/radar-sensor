#!/usr/bin/env python3
"""Build the README images from a SIMULATED scene (no real measurement, no personal data).

    .venv/bin/python docs/img/make_figures.py

Writes into docs/img/:
  hero-dark.png, hero-light.png   room map, chest motion, breathing/pulse from the firmware DSP (simulation only)
  banner.svg                      title banner (adapts to light/dark)
  social-preview.png              1280x640 card for GitHub "Social preview" (upload by hand in the repo settings)

The scene is host/vital_sim.py scenario `beweg_hr`: a seated person at 0.64 m, table, furniture and a wall;
10 s of upper-body movement at 60-70 s while the pulse rises from 70 to 85 /min. The firmware DSP header
(firmware/esp32s3/src/a121_esp_dsp.h) runs on the PC on these frames, exactly as the test bench does.
"""
from __future__ import annotations

import base64
import pathlib
import sys
import tempfile

import matplotlib

matplotlib.use("Agg")
import matplotlib.patheffects as pe  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.gridspec import GridSpec  # noqa: E402

OUT = pathlib.Path(__file__).resolve().parent
REPO = OUT.parent.parent
sys.path.insert(0, str(REPO / "host"))
import vital_sim as vs  # noqa: E402

SCENARIO = "beweg_hr"
MOVE = (60.0, 70.0)

THEMES = {
    "dark": dict(bg="#0d1117", fg="#e6edf3", muted="#8b949e", grid="#30363d", card="#161b22",
                 breath="#58a6ff", pulse="#ff7b72", truth="#e6edf3", move="#d29922", cmap="magma"),
    "light": dict(bg="#ffffff", fg="#1f2328", muted="#59636e", grid="#d1d9e0", card="#f6f8fa",
                  breath="#0969da", pulse="#cf222e", truth="#1f2328", move="#9a6700", cmap="magma"),
}


def load_scene():
    """Record the scenario with the firmware DSP and decode frames, vital reports and the simulated strap."""
    with tempfile.TemporaryDirectory() as d:
        folder = vs.record_scenario(SCENARIO, pathlib.Path(d) / "rec")
        lines = (folder / "stream.log").read_text().splitlines()
        strap = np.genfromtxt(folder / "ref_hr.csv", delimiter=",", names=True)
    frames, vital = [], []
    for ln in lines:
        p = ln.split(",")
        if p[0] == "@RAW":
            iq = np.frombuffer(base64.b64decode(p[4]), "<i2").astype(float).reshape(-1, 2)
            frames.append(iq[:, 0] + 1j * iq[:, 1])
        elif p[0] == "@VITAL":
            vital.append([float(x) for x in p[1:4]])  # t_ms, breathing /min, pulse /min (fused)
    X = np.array(frames)
    vital = np.array(vital)
    t = np.arange(len(X)) / vs.FS
    zoom = X[:, 1:1 + vs.ZOOM_N]
    room = X[:, 1 + vs.ZOOM_N:1 + vs.ZOOM_N + vs.ROOM_N]
    room_m = (vs.ROOM_START_PT + np.arange(vs.ROOM_N) * vs.ROOM_STEP) * vs.STEP_M
    return t, zoom, room, room_m, vital, strap


def echo_map(room, fs, avg_s=0.5):
    """Echo strength per range bin, averaged to avg_s columns, in dB over the noise floor."""
    n = int(avg_s * fs)
    a = np.abs(room)
    cols = a[: len(a) // n * n].reshape(-1, n, a.shape[1]).mean(axis=1)
    db = 20 * np.log10(cols + 1e-9)
    return db - np.percentile(db, 20)


def band(x, fs, lo, hi):
    """Zero-phase band-pass via FFT mask (good enough for a picture)."""
    f = np.fft.rfftfreq(len(x), 1 / fs)
    X = np.fft.rfft(x)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, len(x))


def chest_motion_mm(zoom):
    """Phase of the strongest zoom point → displacement in mm, mean removed."""
    k = np.argmax(np.mean(np.abs(zoom), axis=0))
    ph = np.unwrap(np.angle(zoom[:, k]))
    mm = -ph * vs.LAM / (4 * np.pi) * 1e3
    return mm - mm.mean()


def style_axes(ax, c):
    ax.set_facecolor(c["card"])
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(c["grid"])
    ax.tick_params(colors=c["muted"], labelsize=8, length=3)
    ax.xaxis.label.set_color(c["muted"])
    ax.yaxis.label.set_color(c["muted"])
    ax.grid(True, color=c["grid"], lw=0.5, alpha=0.7)


def hero(scene, theme: str):
    c = THEMES[theme]
    t, zoom, room, room_m, vital, strap = scene
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9})
    fig = plt.figure(figsize=(12, 6.2), dpi=150, facecolor=c["bg"])
    gs = GridSpec(2, 2, figure=fig, width_ratios=[1.35, 1], height_ratios=[1, 1], wspace=0.22, hspace=0.5,
                  left=0.06, right=0.985, top=0.8, bottom=0.09)

    fig.text(0.06, 0.95, "What the radar sees", color=c["fg"], fontsize=17, fontweight="bold")
    fig.text(0.06, 0.9, "Simulated scene: person at 0.64 m moves for 10 s, pulse rises 70 → 85 /min. "
             "Values computed by the ESP32 firmware DSP, run on a PC.", color=c["muted"], fontsize=9.5)

    # Room map
    ax = fig.add_subplot(gs[:, 0])
    style_axes(ax, c)
    ax.grid(False)
    em = echo_map(room, vs.FS)
    im = ax.imshow(em.T, origin="lower", aspect="auto", cmap=c["cmap"], vmin=0, vmax=np.percentile(em, 99.8),
                   extent=[0, t[-1], room_m[0], room_m[-1]], interpolation="bilinear")
    ax.set_ylim(0.3, 2.5)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("distance from sensor (m)")
    ax.set_title("Room scan: echo strength along the beam", color=c["fg"], fontsize=10.5, loc="left", pad=8)
    outline = [pe.withStroke(linewidth=2.5, foreground="#000000")]
    for y, label in [(0.45, "table"), (0.64, "person"), (1.30, "furniture"), (2.20, "wall")]:
        ax.annotate(label, xy=(2, y), xytext=(0, 9), textcoords="offset points", ha="left", va="bottom",
                    color="#ffffff", fontsize=8.5, path_effects=outline)
    for x in MOVE:
        ax.axvline(x, color=c["move"], lw=1, ls=(0, (3, 2)))
    ax.text(np.mean(MOVE), 2.42, "movement", ha="center", va="top", color=c["move"], fontsize=8,
            path_effects=outline)
    cb = fig.colorbar(im, ax=ax, fraction=0.035, pad=0.015)
    cb.outline.set_visible(False)
    cb.ax.tick_params(colors=c["muted"], labelsize=7)
    cb.set_label("echo (dB over noise)", color=c["muted"], fontsize=8)

    # Chest motion, 12 s excerpt
    ax = fig.add_subplot(gs[0, 1])
    style_axes(ax, c)
    x = chest_motion_mm(zoom)
    beat = band(x, vs.FS, 0.8, 3.0)
    k = (t >= 20) & (t < 32)
    ax.plot(t[k], x[k] - x[k].mean(), color=c["breath"], lw=1.4)
    ax.plot(t[k], 10 * beat[k] - 2.9, color=c["pulse"], lw=1.2)
    ax.text(20.15, 1.42, "chest motion (mostly breathing)", color=c["breath"], fontsize=8, va="bottom")
    ax.text(20.15, -1.72, "heartbeat part: band-pass 0.8–3 Hz, ×10", color=c["pulse"], fontsize=8, va="bottom")
    ax.set_xlim(20, 32)
    ax.set_ylim(-4.0, 1.95)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("mm")
    ax.set_title("Phase of the chest echo: breathing and heartbeat", color=c["fg"], fontsize=10.5,
                 loc="left", pad=8)

    # Rates over time
    ax = fig.add_subplot(gs[1, 1])
    style_axes(ax, c)
    ax.axvspan(*MOVE, color=c["move"], alpha=0.18, lw=0)
    ax.plot(strap["rx_ms"] / 1000, strap["hr_bpm"], color=c["truth"], lw=1, ls=(0, (3, 2)), alpha=0.6,
            label="pulse, simulated truth")
    tv = vital[:, 0] / 1000
    gap = np.r_[False, np.diff(tv) > 1.5]
    for col, key, label in [(2, "pulse", "pulse candidate (DSP)"), (1, "breath", "breathing (DSP)")]:
        y = vital[:, col].copy()
        y[gap] = np.nan
        ax.plot(np.where(gap, np.nan, tv), y, color=c[key], lw=1.8, label=label)
    ax.set_xlim(0, t[-1])
    ax.set_ylim(0, 100)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("per minute")
    ax.set_title("Breathing and pulse reported every second", color=c["fg"], fontsize=10.5, loc="left", pad=8)
    leg = ax.legend(loc="center left", bbox_to_anchor=(0.0, 0.47), fontsize=7.5, frameon=False, ncol=1)
    for txt in leg.get_texts():
        txt.set_color(c["muted"])

    fig.text(0.985, 0.955, "Simulation only · not a medical device", ha="right", color=c["muted"], fontsize=8.5)
    out = OUT / f"hero-{theme}.png"
    fig.savefig(out, facecolor=c["bg"])
    plt.close(fig)
    return out


def wave_path(x0, x1, y0, amp, n=600, breaths=3.0, beats=13.0):
    """SVG path of a breathing wave with small heartbeat bumps."""
    xs = np.linspace(x0, x1, n)
    u = (xs - x0) / (x1 - x0)
    y = np.sin(2 * np.pi * breaths * u) + 0.22 * np.exp(-((np.mod(beats * u, 1.0) - 0.3) / 0.05) ** 2)
    ys = y0 - amp * y
    return "M" + " L".join(f"{a:.1f},{b:.1f}" for a, b in zip(xs, ys))


def banner():
    arcs = "\n".join(
        f'    <path d="M {120 + r * 0.5:.1f} {160 - r * 0.866:.1f} A {r} {r} 0 0 1 {120 + r * 0.5:.1f} '
        f'{160 + r * 0.866:.1f}" class="arc" style="opacity:{0.85 - i * 0.13:.2f}"/>'
        for i, r in enumerate([40, 70, 100, 130, 160, 190]))
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 320" role="img" aria-label="radar-sensor: 60 GHz radar for breathing and pulse">
  <style>
    .bg {{ fill: #f6f8fa; }} .title {{ fill: #1f2328; }} .sub {{ fill: #59636e; }}
    .arc {{ fill: none; stroke: #0969da; stroke-width: 3; stroke-linecap: round; }}
    .wave {{ fill: none; stroke: #cf222e; stroke-width: 3.5; stroke-linejoin: round; }}
    .chip {{ fill: #0969da; }} .chipline {{ stroke: #f6f8fa; stroke-width: 2; }}
    @media (prefers-color-scheme: dark) {{
      .bg {{ fill: #161b22; }} .title {{ fill: #e6edf3; }} .sub {{ fill: #8b949e; }}
      .arc {{ stroke: #58a6ff; }} .wave {{ stroke: #ff7b72; }} .chip {{ fill: #58a6ff; }} .chipline {{ stroke: #161b22; }}
    }}
    text {{ font-family: -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; }}
  </style>
  <rect class="bg" width="1280" height="320" rx="24"/>
  <rect class="chip" x="88" y="128" width="40" height="64" rx="8"/>
  <line class="chipline" x1="98" y1="146" x2="118" y2="146"/><line class="chipline" x1="98" y1="160" x2="118" y2="160"/>
  <line class="chipline" x1="98" y1="174" x2="118" y2="174"/>
{arcs}
  <text class="title" x="320" y="150" font-size="68" font-weight="700">radar-sensor</text>
  <text class="sub" x="322" y="200" font-size="26">60 GHz radar that watches breathing and pulse</text>
  <text class="sub" x="322" y="240" font-size="20">Acconeer A121 · ESP32-S3 · Android · Python</text>
  <path class="wave" d="{wave_path(1010, 1225, 160, 30, breaths=2.0, beats=9.0)}"/>
</svg>
"""
    (OUT / "banner.svg").write_text(svg)


def social_preview(scene):
    c = THEMES["dark"]
    t, zoom, room, room_m, vital, strap = scene
    fig = plt.figure(figsize=(12.8, 6.4), dpi=100, facecolor=c["bg"])
    ax = fig.add_axes([0.52, 0.12, 0.44, 0.76])
    em = echo_map(room, vs.FS)
    ax.imshow(em.T, origin="lower", aspect="auto", cmap=c["cmap"], vmin=0, vmax=np.percentile(em, 99.8),
              extent=[0, t[-1], room_m[0], room_m[-1]], interpolation="bilinear")
    ax.set_ylim(0.3, 2.5)
    ax.axis("off")
    fig.text(0.06, 0.62, "radar-sensor", color=c["fg"], fontsize=48, fontweight="bold")
    fig.text(0.06, 0.50, "60 GHz radar that watches\nbreathing and pulse", color=c["muted"], fontsize=22,
             linespacing=1.3, va="top")
    fig.text(0.06, 0.20, "Acconeer A121 · ESP32-S3 · Android · Python", color=c["breath"], fontsize=15)
    fig.savefig(OUT / "social-preview.png", facecolor=c["bg"])
    plt.close(fig)


def main():
    scene = load_scene()
    for theme in THEMES:
        print(hero(scene, theme))
    banner()
    social_preview(scene)
    print(OUT / "banner.svg", OUT / "social-preview.png")


if __name__ == "__main__":
    main()
