package de.timo.radarsensor.record

import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Sound source of a metronome run (a ToneGenerator in the app). */
interface Beeper {
    fun beep()

    fun close()
}

/**
 * One metronome run until the coroutine is cancelled: opens its sound source itself and closes it in finally, so that
 * after stop() no tone goes to a released source (not even with stop() + start() directly in a row).
 * Tone k at [MetronomeSchedule.tickNanos]; after each tone [onTick] with the time after the tone. Missed tones (run
 * stalled) are skipped, not made up.
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
