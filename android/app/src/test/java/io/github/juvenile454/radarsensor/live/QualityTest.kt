package io.github.juvenile454.radarsensor.live

import io.github.juvenile454.radarsensor.protocol.Dsp
import io.github.juvenile454.radarsensor.protocol.LineParser
import io.github.juvenile454.radarsensor.protocol.MeasState
import io.github.juvenile454.radarsensor.protocol.Vital
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** V5.1: quality state of the pulse and deviation against the chest strap. */
class QualityTest {
    private fun vital(conf: Float) = LineParser.parse("@VITAL,1000,15.0,72.0,72.0,71.0,72.5,830,40,$conf,2000,60,20.0,1.2,650,1") as Vital

    private fun dsp(motion: Float) = Dsp(1000, true, 5, 80f, 300000, motion, false)

    @Test fun grauBeiKleinerKonfidenz() {
        val q = pulseQuality(vital(40f), dsp(0f), MeasState(1000, MeasState.VALID, 20000))
        assertEquals(true, q.grey)
        assertEquals("Konfidenz 40 %", q.reason)
        assertEquals(false, pulseQuality(vital(80f), dsp(0f), MeasState(1000, MeasState.VALID, 20000)).grey)
    }

    @Test fun grauBeiBewegung() {
        assertEquals("Bewegung", pulseQuality(vital(90f), dsp(0f), MeasState(1000, MeasState.MOTION, 3000)).reason)
        assertEquals(true, pulseQuality(vital(90f), dsp(45f), MeasState(1000, MeasState.VALID, 3000)).grey)
    }

    @Test fun zustandsText() {
        assertEquals("Erfassung läuft 4 s", pulseQuality(null, null, MeasState(1000, MeasState.ACQUIRE, 4400)).stateText)
        assertEquals("gültig seit 20 s", pulseQuality(vital(80f), dsp(0f), MeasState(1000, MeasState.VALID, 20000)).stateText)
        assertEquals("Atem angehalten (Apnoe)", pulseQuality(vital(80f), dsp(0f), MeasState(1000, MeasState.APNEA, 0)).stateText)
        // without @STATE (firmware before V3.5): from the validity of the report
        assertEquals("gültig", pulseQuality(vital(80f), dsp(0f), null).stateText)
        assertEquals(true, pulseQuality(null, null, null).grey)
    }

    @Test fun grauBeiUngueltigemReportAuchInApnoe() {
        // Final review C1: apnea with an invalid report (@DSP ok=0, no new @VITAL) — the old value is not valid
        val alt = vital(80f) // t 1000
        val ungueltig = Dsp(5000, false, 5, 80f, 300000, 0f, false)
        val q = pulseQuality(alt, ungueltig, MeasState(5000, MeasState.APNEA, 3000))
        assertEquals(true, q.grey)
        assertEquals("ungültig", q.reason)
        assertEquals(true, pulseQuality(alt, ungueltig, MeasState(5000, MeasState.VALID, 3000)).grey)
    }

    @Test fun grauBeiAltemWert() {
        // valid report, but its @VITAL is missing: the displayed value comes from an earlier report
        val q = pulseQuality(vital(80f), Dsp(5000, true, 5, 80f, 300000, 0f, false), MeasState(5000, MeasState.VALID, 9000))
        assertEquals(true, q.grey)
        assertEquals("veraltet", q.reason)
        // the same report pair (@DSP shortly before @VITAL) is not stale
        assertEquals(false, pulseQuality(vital(80f), Dsp(998, true, 5, 80f, 300000, 0f, false), MeasState(1000, MeasState.VALID, 9000)).grey)
    }

    @Test fun abweichungGegenGurt() {
        val d = deviation(floatArrayOf(10f, 11f, 12f, 40f), floatArrayOf(72f, 74f, 0f, 80f),
                          floatArrayOf(9.6f, 10.9f, 12.1f), floatArrayOf(70f, 71f, 72f), windowS = 60f)!!
        // 12 s: radar without a value (0); 40 s: no chest strap value within 2 s
        assertEquals(3f, d.last, 1e-6f)
        assertEquals(2.5f, d.medianAbs, 1e-6f)
        assertEquals(2, d.n)
        assertNull(deviation(floatArrayOf(10f), floatArrayOf(72f), floatArrayOf(), floatArrayOf(), 60f))
    }
}
