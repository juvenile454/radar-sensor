package de.timo.radarsensor.dsp

import de.timo.radarsensor.protocol.LineParser
import de.timo.radarsensor.protocol.Vital
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/**
 * Echte Bibliothek radardsp_live (am PC übersetzt mit A121_DSP_HEAVY) mit einem synthetischen iq3-raw-Log aus
 * host/vital_sim.py. Ohne Bibliothek oder Log (normaler App-Build) übersprungen; am Handy ist das keine Prüfung.
 */
class LiveDspNativeTest {
    private fun log(): File? = System.getProperty("radar.liveLog")?.let(::File)?.takeIf { it.exists() }

    private fun run(lines: List<String>): List<String> {
        val out = mutableListOf<String>()
        val p = PhoneDsp(LiveDsp) { out += it }
        p.setEnabled(true)
        lines.forEach(p::offer)
        p.setEnabled(false)
        return out
    }

    @Test fun rechnetVitalwerteAusRohdaten() {
        val f = log()
        assumeTrue("radar.liveLog fehlt", f != null)
        assumeTrue("radardsp_live nicht geladen: ${LiveDsp.loadError}", LiveDsp.available)
        val lines = f!!.readLines()
        val cpu0 = LiveDsp.cpuNanos()
        val out = run(lines)
        val vit = out.map(LineParser::parse).filterIsInstance<Vital>()
        assertTrue("nur ${vit.size} @VITAL", vit.size > 5)
        assertTrue(vit.all { it.hrFusedBpm in 40f..180f })
        assertTrue(LiveDsp.cpuNanos() > cpu0)
        // Neubeginn: gleiche Eingabe, gleiche Ausgabe (keine Reste vom ersten Lauf)
        assertEquals(out, run(lines))
    }
}
