package io.github.juvenile454.radarsensor.record

/**
 * Tempo of the metronome breathing (V0.2): tone k lies at start + k · 60 / tempo, without drift over long recordings.
 * Tempos as in measurement plan R3 (docs/EXPERIMENTS.md): 6 / 10 / 15 / 20 /min.
 */
class MetronomeSchedule(
    val startNanos: Long,
    val ratePerMin: Int,
) {
    init {
        require(ratePerMin in RATES) { "Takt $ratePerMin /min nicht vorgesehen ($RATES)" }
    }

    val periodNanos: Long = 60_000_000_000L / ratePerMin

    fun tickNanos(k: Int): Long = startNanos + k.toLong() * periodNanos

    /** Index of the next tone after nowNanos (tone 0 = start). */
    fun nextIndex(nowNanos: Long): Int = if (nowNanos < startNanos) 0 else ((nowNanos - startNanos) / periodNanos + 1).toInt()

    companion object {
        val RATES = listOf(6, 10, 15, 20)
    }
}
