package de.timo.radarsensor.dsp

import android.os.SystemClock
import java.io.File

/**
 * A5: Recomputes an `iq3 … raw` recording on the phone with the firmware's DSP header (NDK, `host/dsp_replay/`) and
 * compares every line with the live output of the ESP32. Writes `dsp_phone.log` (recomputed lines in the same
 * format, can be evaluated with `host/a121_esp_ultimate.py --mode plot`) and `dsp_phone_report.txt` into the recording folder.
 */
object DspReplay {
    init {
        System.loadLibrary("radardsp")
    }

    data class Result(
        val code: Int,
        val ms: Long,
        val report: String,
    ) {
        val summary: String
            get() =
                report.lineSequence().firstOrNull { it.startsWith("ERGEBNIS") }
                    ?: if (code == 2) "Datei nicht lesbar" else "kein Ergebnis"
    }

    private external fun nativeRun(
        inPath: String,
        outPath: String,
        reportPath: String,
    ): Int

    /** Blocks (a few seconds for a 30 min recording); do not call on the UI thread. */
    fun run(dir: File): Result {
        val rep = File(dir, "dsp_phone_report.txt")
        val t0 = SystemClock.elapsedRealtime()
        val code = nativeRun(File(dir, "stream.log").path, File(dir, "dsp_phone.log").path, rep.path)
        val ms = SystemClock.elapsedRealtime() - t0
        val text = if (rep.exists()) rep.readText() else ""
        rep.appendText("Rechenzeit Handy: $ms ms\n")
        return Result(code, ms, text)
    }
}
