"""Reference measurement of a single target (e.g. wall) with the A121.

1) Distance detector: 100 frames, distances grouped in 10 cm clusters.
2) Sparse IQ fine (profile 2, HWAAS 32, 2.5 mm steps): echo strength, SNR, echo width.
Result as .npz to ~/Data/radar-sensor/.

Example:
  sg dialout -c ".venv/bin/python host/a121_zielmessung.py \
      /dev/serial/by-id/usb-1a86_USB_Dual_Serial_<SERIAL>-if00 \
      ~/Data/radar-sensor/2026-10-30_ref_wand_0p57m_nackt.npz \
      'A121 ohne Kappe, Backsteinwand 570 mm'"

Known: right after a session has ended, the next connection often fails with
`LinkError: recv timeout`; just start again.
"""
import json
import sys
import time

import numpy as np
from acconeer.exptool import a121
from acconeer.exptool.a121.algo.distance import Detector, DetectorConfig

DET_START_M, DET_END_M = 0.4, 2.5
IQ_START_POINT, IQ_NUM_POINTS = 160, 840  # 0.40–2.50 m at 2.5 mm/point
POINT_M = 0.0025


def main(port: str, out: str, label: str) -> None:
    client = a121.Client.open(serial_port=port)
    print("rss_version:", client.server_info.rss_version)

    # 1) Distance detector (targets come sorted by strength, not by distance)
    det = Detector(client=client, sensor_ids=[1],
                   detector_config=DetectorConfig(start_m=DET_START_M, end_m=DET_END_M))
    det.calibrate_detector()
    det.start()
    nearest, allpk = [], []
    for _ in range(100):
        r = det.get_next()[1]
        d = [float(x) for x in (r.distances if r.distances is not None else [])]
        s = [float(x) for x in (r.strengths if r.strengths is not None else [])]
        if d:
            nearest.append(min(d))
        allpk.append({"d": d, "s": s})
    det.stop()

    nearest = np.array(nearest)
    alld = np.array([x for p in allpk for x in p["d"]])
    print(f"DETEKTOR: 100 Frames, mit Ziel {len(nearest)}")
    for lo in np.arange(DET_START_M, DET_END_M, 0.1):
        m = (alld >= lo) & (alld < lo + 0.1)
        if m.sum():
            print(f"  Cluster {lo:.1f}-{lo + 0.1:.1f} m: n={m.sum()} median={np.median(alld[m]):.4f} m "
                  f"std={alld[m].std() * 1000:.1f} mm")

    # 2) Sparse IQ fine
    cfg = a121.SensorConfig(start_point=IQ_START_POINT, num_points=IQ_NUM_POINTS, step_length=1,
                            profile=a121.Profile.PROFILE_2, hwaas=32, sweeps_per_frame=4)
    client.setup_session(cfg)
    client.start_session()
    frames = np.array([client.get_next().frame for _ in range(50)])
    client.stop_session()
    client.close()

    amp = np.abs(frames).mean(axis=(0, 1))
    dist = (IQ_START_POINT + np.arange(IQ_NUM_POINTS)) * POINT_M
    k = int(np.argmax(amp))
    noise = float(np.median(amp))
    lo, hi = k, k
    while lo > 0 and amp[lo] > amp[k] / 2:
        lo -= 1
    while hi < len(amp) - 1 and amp[hi] > amp[k] / 2:
        hi += 1
    print(f"SPARSE IQ: Peak {amp[k]:.0f} bei {dist[k]:.4f} m (roh, ohne Offset-Kalibrierung), "
          f"SNR {20 * np.log10(amp[k] / noise):.1f} dB, FWHM {(hi - lo) * POINT_M * 1000:.0f} mm")

    np.savez_compressed(out, frames=frames, distances_m=dist, detector_nearest=nearest,
                        detector_all=json.dumps(allpk),
                        meta=json.dumps({"setup": label, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                                         "sensor_config": str(cfg)}))
    print("gespeichert:", out)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
