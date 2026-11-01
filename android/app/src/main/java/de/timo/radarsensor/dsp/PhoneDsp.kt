package de.timo.radarsensor.dsp

/**
 * V5.2: Handy-DSP ein/aus und Zuführung der empfangenen Zeilen. Merkt sich `@IQ3_START` und den letzten `@REZOOM` des
 * laufenden Streams, damit Einschalten mitten im Stream gleich mit dem richtigen Zoom-Fenster rechnet (erster Wert
 * nach ≈ 6 s wie beim Start). Nicht threadsicher: alle Aufrufe aus einem Thread (RadarController: eigener
 * Rechen-Thread, damit der Empfang nicht wartet).
 */
class PhoneDsp(
    private val engine: DspEngine,
    private val onResult: (String) -> Unit,
) {
    var enabled = false
        private set

    /** CPU-Anteil der Handy-DSP an einem Kern in % über die letzte Sekunde ([sampleCpu]); null = aus oder noch keiner. */
    var cpuPct: Float? = null
        private set

    private var start: String? = null
    private var rezoom: String? = null
    private var lastWallNs = -1L
    private var lastCpuNs = 0L

    fun setEnabled(on: Boolean) {
        val want = on && engine.available
        if (want == enabled) return
        enabled = want
        cpuPct = null
        lastWallNs = -1L
        if (!enabled) return
        engine.reset()
        start?.let(::feed)
        rezoom?.let(::feed)
    }

    fun offer(line: String) {
        when {
            line.startsWith("@IQ3_START,") -> {
                start = line
                rezoom = null
            }
            line.startsWith("@REZOOM,") -> rezoom = line
            line.trimEnd() == "@END" -> {
                start = null
                rezoom = null
            }
        }
        if (enabled) feed(line)
    }

    /** Einmal je Sekunde mit der Wanduhr (ns): CPU-Anteil seit dem letzten Aufruf in % eines Kerns. */
    fun sampleCpu(wallNs: Long): Float? {
        if (!enabled) return null
        val cpu = engine.cpuNanos()
        val pct = if (lastWallNs >= 0 && wallNs > lastWallNs) 100f * (cpu - lastCpuNs) / (wallNs - lastWallNs) else null
        lastWallNs = wallNs
        lastCpuNs = cpu
        if (pct != null) cpuPct = pct
        return pct
    }

    private fun feed(line: String) {
        engine.line(line)?.split('\n')?.forEach(onResult)
    }
}
