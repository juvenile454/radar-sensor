package io.github.juvenile454.radarsensor.dsp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PhoneDspTest {
    /** Replacement for the native library (the real one is checked by LiveDspNativeTest): remembers the lines, answers with fixed values. */
    private class Engine(
        override val available: Boolean = true,
    ) : DspEngine {
        val fed = mutableListOf<String>()
        var resets = 0
        var cpu = 0L
        var answer: (String) -> String? = { null }

        override fun reset() {
            resets++
            fed.clear()
        }

        override fun line(line: String): String? {
            fed += line
            return answer(line)
        }

        override fun cpuNanos(): Long = cpu
    }

    private val start = "@IQ3_START,500,560,60.0,0"
    private val rezoom = "@REZOOM,687,747"

    @Test fun ausgeschaltetRechnetNichts() {
        val e = Engine()
        val out = mutableListOf<String>()
        val p = PhoneDsp(e) { out += it }
        p.offer(start)
        p.offer("@RAW,0,572,105,AAAA")
        assertTrue(e.fed.isEmpty())
        assertTrue(out.isEmpty())
        assertFalse(p.enabled)
    }

    @Test fun einschaltenMittenImStreamSpieltStartUndRezoomNach() {
        val e = Engine()
        val p = PhoneDsp(e) {}
        p.offer(start)
        p.offer("@IQ3,8,14,1.0,0,1.0,0,0,572")
        p.offer(rezoom)
        p.offer("@RAW,1,589,105,AAAA")
        p.setEnabled(true)
        assertEquals(1, e.resets)
        assertEquals(listOf(start, rezoom), e.fed)
        p.offer("@RAW,2,606,105,AAAA")
        assertEquals("@RAW,2,606,105,AAAA", e.fed.last())
    }

    @Test fun neuerStreamVergisstAltenRezoom() {
        val e = Engine()
        val p = PhoneDsp(e) {}
        p.offer(start)
        p.offer(rezoom)
        p.offer("@IQ3_START,520,580,60.0,0")
        p.setEnabled(true)
        assertEquals(listOf("@IQ3_START,520,580,60.0,0"), e.fed)
    }

    @Test fun nachStreamEndeKeinNachspielen() {
        val e = Engine()
        val p = PhoneDsp(e) {}
        p.offer(start)
        p.offer("@END")
        p.setEnabled(true)
        assertTrue(e.fed.isEmpty())
        assertTrue(p.enabled)
    }

    @Test fun ergebnisseZeilenweise() {
        val e = Engine()
        e.answer = { if (it.startsWith("@DSP")) "@DSP,1000,1,5\n@VITAL,1000,15.0,70.0" else null }
        val out = mutableListOf<String>()
        val p = PhoneDsp(e) { out += it }
        p.setEnabled(true)
        p.offer("@DSP,1000,1,5,72.0,300000")
        assertEquals(listOf("@DSP,1000,1,5", "@VITAL,1000,15.0,70.0"), out)
    }

    @Test fun ohneBibliothekBleibtAus() {
        val e = Engine(available = false)
        val p = PhoneDsp(e) {}
        p.offer(start)
        p.setEnabled(true)
        assertFalse(p.enabled)
        assertEquals(0, e.resets)
        assertNull(p.sampleCpu(1_000_000_000L))
    }

    @Test fun ausschaltenStopptZufuhr() {
        val e = Engine()
        val p = PhoneDsp(e) {}
        p.setEnabled(true)
        p.setEnabled(false)
        p.offer(start)
        assertTrue(e.fed.isEmpty())
    }

    @Test fun cpuAnteilJeSekunde() {
        val e = Engine()
        val p = PhoneDsp(e) {}
        p.setEnabled(true)
        assertNull(p.sampleCpu(1_000_000_000L)) // first value: no interval yet
        e.cpu = 50_000_000L
        assertEquals(5f, p.sampleCpu(2_000_000_000L)!!, 1e-4f)
        e.cpu = 60_000_000L
        assertEquals(1f, p.sampleCpu(3_000_000_000L)!!, 1e-4f)
        assertEquals(1f, p.cpuPct!!, 1e-4f)
        p.setEnabled(false)
        assertNull(p.cpuPct)
    }
}
