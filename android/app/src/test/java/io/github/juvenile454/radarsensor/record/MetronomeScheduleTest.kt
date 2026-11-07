package io.github.juvenile454.radarsensor.record

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** V0.2: tempo of the metronome breathing without drift (every tone at start + k · 60 / tempo), tempos 6/10/15/20 /min. */
class MetronomeScheduleTest {
    @Test fun ticksWithoutDrift() {
        val s = MetronomeSchedule(startNanos = 5_000_000_000L, ratePerMin = 15)
        assertEquals(5_000_000_000L, s.tickNanos(0))
        assertEquals(9_000_000_000L, s.tickNanos(1))
        assertEquals(5_000_000_000L + 1000L * 4_000_000_000L, s.tickNanos(1000))
    }

    @Test fun nextTickAfterNow() {
        val s = MetronomeSchedule(startNanos = 0L, ratePerMin = 10)
        assertEquals(1, s.nextIndex(nowNanos = 1L))
        assertEquals(1, s.nextIndex(nowNanos = 6_000_000_000L - 1))
        assertEquals(2, s.nextIndex(nowNanos = 6_000_000_000L))
    }

    @Test fun onlyPlannedRates() {
        assertEquals(listOf(6, 10, 15, 20), MetronomeSchedule.RATES)
        assertThrows(IllegalArgumentException::class.java) { MetronomeSchedule(0L, 12) }
    }
}
