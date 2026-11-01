package de.timo.radarsensor.record

/**
 * Takt der Metronom-Atmung (V0.2): Ton k liegt auf start + k · 60 / Takt, ohne Drift über lange Aufnahmen. Takte wie
 * im Messplan R3 (docs/EXPERIMENTE.md): 6 / 10 / 15 / 20 /min.
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

    /** Index des nächsten Tons nach nowNanos (Ton 0 = Start). */
    fun nextIndex(nowNanos: Long): Int = if (nowNanos < startNanos) 0 else ((nowNanos - startNanos) / periodNanos + 1).toInt()

    companion object {
        val RATES = listOf(6, 10, 15, 20)
    }
}
