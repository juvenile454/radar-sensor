package de.timo.radarsensor.record

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Sequence of a metronome run (Metronome.kt) with a virtual clock: the sound source belongs to the run. */
class MetronomeLoopTest {
    private class FakeBeeper(
        val log: MutableList<String>,
        val name: String,
    ) : Beeper {
        var closed = false

        override fun beep() {
            check(!closed) { "$name: Ton nach close()" }
            log += "$name beep"
        }

        override fun close() {
            check(!closed) { "$name: doppelt geschlossen" }
            closed = true
            log += "$name close"
        }
    }

    private var t = 0L
    private val sleep: suspend (Long) -> Unit = { ms ->
        t += ms * 1_000_000L
        yield()
    }

    @Test
    fun toeneAufDemTaktUndSchliessenBeimAbbruch() =
        runBlocking {
            val log = mutableListOf<String>()
            val ticks = mutableListOf<Long>()
            val sched = MetronomeSchedule(500_000_000L, 20)
            lateinit var job: Job
            var beeper: FakeBeeper? = null
            job =
                launch {
                    runMetronome(sched, { t }, sleep, { FakeBeeper(log, "a").also { beeper = it } }) { n ->
                        ticks += n
                        if (ticks.size == 3) job.cancel()
                    }
                }
            job.join()
            assertEquals(listOf(sched.tickNanos(0), sched.tickNanos(1), sched.tickNanos(2)), ticks)
            assertEquals(listOf("a beep", "a beep", "a beep", "a close"), log)
            assertTrue(beeper!!.closed)
        }

    @Test
    fun abbruchVorDemStartOeffnetNichts() =
        runBlocking {
            var opened = false
            val job =
                launch(start = CoroutineStart.LAZY) {
                    runMetronome(MetronomeSchedule(0L, 6), { t }, sleep, { opened = true; FakeBeeper(mutableListOf(), "x") }) {}
                }
            job.cancelAndJoin()
            assertFalse(opened)
        }

    @Test
    fun neuerLaufNachAbbruchBenutztNieDieAlteQuelle() =
        runBlocking {
            // like stop() + start(): the old run closes its own source, the new one opens its own
            val log = mutableListOf<String>()
            val a = launch { runMetronome(MetronomeSchedule(0L, 15), { t }, sleep, { FakeBeeper(log, "a") }) {} }
            yield()
            a.cancel()
            val b = launch { runMetronome(MetronomeSchedule(t, 15), { t }, sleep, { FakeBeeper(log, "b") }) {} }
            repeat(5) { yield() }
            b.cancelAndJoin()
            a.join()
            assertEquals(1, log.count { it == "a close" })
            assertEquals(1, log.count { it == "b close" })
            assertTrue(log.indexOf("a close") > log.indexOfFirst { it == "a beep" })
            assertFalse(log.drop(log.indexOf("a close") + 1).contains("a beep"))
            assertTrue(log.contains("b beep"))
        }
}
