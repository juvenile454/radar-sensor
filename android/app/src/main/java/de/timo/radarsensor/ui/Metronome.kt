package de.timo.radarsensor.ui

import android.media.AudioManager
import android.media.ToneGenerator
import android.os.SystemClock
import de.timo.radarsensor.record.Beeper
import de.timo.radarsensor.record.MetronomeSchedule
import de.timo.radarsensor.record.runMetronome
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Metronome breathing (V0.2): a short tone per breath for inhaling, tempo 6 / 10 / 15 / 20 /min. The tones lie without
 * drift on [MetronomeSchedule]; after each tone it calls [onTick] with the time of the tone (elapsedRealtimeNanos) — the
 * controller writes the marker "ein" from it. The output latency of the tone is a fixed offset, the rate is correct.
 */
class Metronome(
    private val scope: CoroutineScope,
    private val onTick: (nanos: Long) -> Unit,
) {
    private var job: Job? = null

    private val _rate = MutableStateFlow<Int?>(null)
    val rate: StateFlow<Int?> = _rate.asStateFlow()

    fun start(ratePerMin: Int) {
        stop()
        val sched = MetronomeSchedule(SystemClock.elapsedRealtimeNanos() + 500_000_000L, ratePerMin)
        _rate.value = ratePerMin
        job =
            scope.launch {
                // The ToneGenerator belongs to the run (runMetronome releases it in finally), stop() only cancels
                runMetronome(sched, SystemClock::elapsedRealtimeNanos, { delay(it) }, { ToneBeeper() }, onTick)
            }
    }

    fun stop() {
        job?.cancel()
        job = null
        _rate.value = null
    }

    private class ToneBeeper : Beeper {
        private val tg = ToneGenerator(AudioManager.STREAM_MUSIC, 80)

        override fun beep() {
            tg.startTone(ToneGenerator.TONE_PROP_BEEP, 150)
        }

        override fun close() = tg.release()
    }
}
