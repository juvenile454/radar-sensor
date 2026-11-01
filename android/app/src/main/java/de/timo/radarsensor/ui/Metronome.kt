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
 * Metronom-Atmung (V0.2): ein kurzer Ton je Atemzug zum Einatmen, Takt 6 / 10 / 15 / 20 /min. Die Töne liegen ohne
 * Drift auf [MetronomeSchedule]; nach jedem Ton ruft [onTick] mit der Zeit des Tons (elapsedRealtimeNanos) — der
 * Controller schreibt daraus den Marker „ein". Die Ausgabelatenz des Tons ist ein fester Versatz, die Rate stimmt.
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
                // ToneGenerator gehört dem Lauf (runMetronome gibt ihn im finally frei), stop() bricht nur ab
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
