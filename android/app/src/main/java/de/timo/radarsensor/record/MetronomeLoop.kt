package de.timo.radarsensor.record

import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Klangquelle eines Metronom-Laufs (in der App ein ToneGenerator). */
interface Beeper {
    fun beep()

    fun close()
}

/**
 * Ein Metronom-Lauf bis zum Abbruch der Coroutine: öffnet seine Klangquelle selbst und schließt sie im finally, damit
 * nach stop() kein Ton auf eine freigegebene Quelle geht (auch nicht bei stop() + start() direkt hintereinander).
 * Ton k auf [MetronomeSchedule.tickNanos]; nach jedem Ton [onTick] mit der Zeit nach dem Ton. Verpasste Töne (Lauf
 * hing) werden übersprungen, nicht nachgeholt.
 */
suspend fun runMetronome(
    sched: MetronomeSchedule,
    now: () -> Long,
    sleep: suspend (ms: Long) -> Unit,
    open: () -> Beeper,
    onTick: (nanos: Long) -> Unit,
) {
    val b = open()
    try {
        var k = 0
        while (true) {
            currentCoroutineContext().ensureActive()
            val waitMs = (sched.tickNanos(k) - now()) / 1_000_000L
            if (waitMs > 0) sleep(waitMs)
            currentCoroutineContext().ensureActive()
            b.beep()
            onTick(now())
            k = sched.nextIndex(now()).coerceAtLeast(k + 1)
        }
    } finally {
        b.close()
    }
}
