package de.timo.radarsensor.dsp

import android.os.SystemClock
import java.io.File

/**
 * A5: Rechnet eine `iq3 … raw`-Aufnahme mit dem DSP-Header der Firmware am Handy neu (NDK, `host/dsp_replay/`) und
 * vergleicht jede Zeile mit der Live-Ausgabe des ESP32. Schreibt `dsp_phone.log` (neu gerechnete Zeilen im selben
 * Format, auswertbar mit `host/a121_esp_ultimate.py --mode plot`) und `dsp_phone_report.txt` in den Aufnahmeordner.
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

    /** Blockiert (bei 30 min Aufnahme einige Sekunden); nicht auf dem UI-Thread aufrufen. */
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
