package de.timo.radarsensor.dsp

/** Rechenkern der Handy-DSP; [LiveDsp] ist die native Fassung, Tests setzen einen Ersatz ein. */
interface DspEngine {
    val available: Boolean

    fun reset()

    /** Eine empfangene Zeile; Rückgabe die dadurch fertigen Handy-Zeilen (durch '\n' getrennt) oder null. */
    fun line(line: String): String?

    /** CPU-Zeit aller [line]-Aufrufe in ns. */
    fun cpuNanos(): Long
}

/**
 * V5.2: Live-DSP am Handy aus `@RAW` (NDK-Bibliothek radardsp_live, `host/dsp_replay/dsp_live.h`): derselbe
 * Firmware-Header wie auf dem ESP32, aber mit `A121_DSP_HEAVY` (Viterbi-Verfolgung und Abzug der Atem-Oberwellen).
 * Rechnet an denselben Stellen wie der ESP32 (an dessen `@DSP`-Zeilen), Zeitstempel vom ESP32. Fehlt die Bibliothek,
 * ist [available] false und [loadError] sagt warum.
 */
object LiveDsp : DspEngine {
    val loadError: String? =
        try {
            System.loadLibrary("radardsp_live")
            null
        } catch (e: UnsatisfiedLinkError) {
            e.message ?: "radardsp_live nicht ladbar"
        }

    override val available: Boolean get() = loadError == null

    private external fun nativeReset()

    private external fun nativeLine(line: String): String?

    private external fun nativeCpuNanos(): Long

    override fun reset() = nativeReset()

    override fun line(line: String): String? = nativeLine(line)

    override fun cpuNanos(): Long = nativeCpuNanos()
}
