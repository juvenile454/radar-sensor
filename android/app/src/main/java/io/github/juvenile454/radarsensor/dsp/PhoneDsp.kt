package io.github.juvenile454.radarsensor.dsp

/**
 * V5.2: phone DSP on/off and feeding of the received lines. Remembers `@IQ3_START` and the last `@REZOOM` of the
 * running stream, so that switching on mid-stream computes with the correct zoom window right away (first value
 * after ≈ 6 s as at start). Not thread-safe: all calls from one thread (RadarController: own
 * compute thread so that receiving does not wait).
 */
class PhoneDsp(
    private val engine: DspEngine,
    private val onResult: (String) -> Unit,
) {
    var enabled = false
        private set

    /** CPU share of the phone DSP on one core in % over the last second ([sampleCpu]); null = off or none yet. */
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

    /** Once per second with the wall clock (ns): CPU share since the last call in % of one core. */
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
