package de.timo.radarsensor.live

import de.timo.radarsensor.protocol.LineParser
import de.timo.radarsensor.reference.HrMeasurement
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LiveStoreTest {
    private fun LiveStore.feed(vararg lines: String) = lines.forEach { apply(it, LineParser.parse(it)) }

    @Test fun frameVerlustUndZeitachse() {
        val s = LiveStore()
        s.feed(
            "@IQ3_START,500,560,60.0,0",
            "@IQ3,8,14,1419.0,0.00,206.6,0,0,572",
            "@IQ3,25,14,1419.0,1.00,206.6,0,1,589",
            // Frames 2 and 3 are missing (lost between ESP32 and app)
            "@IQ3,80,14,1419.0,2.00,206.6,0,4,639",
            "@IQ3,95,14,1419.0,3.00,206.6,1,5,656",
        )
        val snap = s.snapshot()
        assertEquals(2L, snap.framesLost)
        assertEquals(1L, snap.frameGaps)
        assertEquals(1L, snap.iqSaturatedFrames)
        assertEquals(StreamMode.IQ3, snap.mode)
        // Time from tick, zero point at the first frame + t_ms
        assertEquals(listOf(0.008f, 0.025f, 0.075f, 0.092f), snap.iqT.toList())
        assertEquals(4, snap.iqDisp.size)
    }

    @Test fun neuerStreamNachEspNeustartLaeuftWeiter() {
        var now = 0L
        val s = LiveStore(nanoTime = { now })
        s.feed("@IQ3_START,500,560,60.0,0", "@IQ3,8,14,1.0,0,1.0,0,0,572", "@IQ3,10008,14,1.0,0,1.0,0,1,10572")
        now += 8_000_000_000L // 8 s unplugged, ESP reboots: counter, tick and t_ms start over
        s.feed("@IQ3_START,500,560,60.0,0", "@IQ3,9,14,1.0,0,1.0,0,0,300", "@IQ3,26,14,1.0,0,1.0,0,1,317")
        val t = s.snapshot().iqT.toList()
        assertEquals(4, t.size)
        assertEquals(10.008f, t[1], 1e-4f)
        assertEquals(18.017f, t[2], 1e-3f) // 10.008 s + 8 s pause + 9 ms
        assertTrue(t[3] > t[2])
        assertEquals(0L, s.snapshot().framesLost)
    }

    @Test fun tickNeustartNachReconnect() {
        val c = TickClock()
        assertEquals(0.008f, c.seconds(572, 8), 1e-6f)
        assertEquals(10.008f, c.seconds(10572, 10_008), 1e-6f)
        // Server restarted: tick starts small; time continues from t_ms, not backwards
        val t = c.seconds(300, 11_500)
        assertEquals(11.5f, t, 1e-6f)
        assertEquals(11.6f, c.seconds(400, 11_610), 1e-6f)
    }

    @Test fun neuKalibrierungUndVerspaeteteFrames() {
        val s = LiveStore()
        s.feed(
            "@IQ3_START,500,560,60.0,0",
            "@IQ3,8,14,1419.0,0.00,206.6,0,0,572,0",
            "@IQ3,25,14,1419.0,0.00,206.6,0,1,589,1",
            "@WARN,calibration_needed",
            "@RECAL,40",
        )
        val snap = s.snapshot()
        assertEquals(1L, snap.iqDelayedFrames)
        assertTrue(snap.events.any { it.kind == EventKind.REZOOM && it.text == "Neu kalibriert" })
    }

    @Test fun atemAngehaltenAlsEreignis() {
        val s = LiveStore()
        s.feed(
            "@IQ3_START,500,560,60.0,0",
            "@DSP,1000,1,5,72.0,300000,0,0,55,0",
            "@DSP,2000,1,5,72.0,300000,0,0,55,2",
            "@DSP,3000,1,5,72.0,300000,0,0,55,2",
            "@DSP,4000,1,5,72.0,300000,0,0,55,0",
        )
        val br = s.snapshot().events.filter { it.kind == EventKind.BREATH }.map { it.text }
        assertEquals(listOf("Atem angehalten", "Atmung wieder da"), br)
        assertEquals(0, s.snapshot().lastDsp?.brState)
    }

    @Test fun ereignisseUndEnde() {
        val s = LiveStore()
        s.feed(
            "@IQ3_START,500,560,60.0,0",
            "@REZOOM,687,747",
            "@WARN,frame_timeout",
            "@RECONNECT,10967,1,1",
            "@END",
        )
        val snap = s.snapshot()
        assertEquals(StreamMode.IDLE, snap.mode)
        assertEquals(687f, snap.zoomStartMm)
        assertEquals(1, snap.reconnects)
        assertEquals(1L, snap.frameTimeouts)
        assertEquals(1, snap.rezooms)
        assertEquals(listOf(EventKind.START, EventKind.REZOOM, EventKind.WARN, EventKind.RECONNECT, EventKind.END), snap.events.map { it.kind })
        // Events end up in the console, @IQ3 does not
        assertTrue(snap.console.contains("@REZOOM,687,747"))
    }

    @Test fun wasserfallBegrenzt() {
        val s = LiveStore(waterfallRows = 3)
        val amp = (1..80).joinToString(",")
        repeat(5) { i -> s.feed("@ROOMPROF,${i * 200},${i * 200},300,30,80,$amp,$amp") }
        val snap = s.snapshot()
        assertEquals(3, snap.waterfall.size)
        assertEquals(80, snap.waterfall[0].size)
        assertEquals(listOf(0.4f, 0.6f, 0.8f), snap.waterfallT.toList())
    }

    @Test fun ringpuffer() {
        val r = FloatRing(3)
        assertTrue(r.last().isNaN())
        for (v in 1..5) r.add(v.toFloat())
        assertEquals(listOf(3f, 4f, 5f), r.toArray().toList())
        assertEquals(5f, r.last())
    }

    /** V0.1/V5.1: chest strap pulse on the time axis of the radar values (last data time + time elapsed since). */
    @Test fun referenzKurveAufDerLiveZeitachse() {
        var now = 0L
        val s = LiveStore(nanoTime = { now })
        s.reference(HrMeasurement(70, emptyList(), true)) // no radar time yet: dropped
        assertEquals(0, s.snapshot().refT.size)
        s.feed("@IQ3_START,500,560,60.0,0", "@IQ3,2000,14,1.0,0,1.0,0,0,2572")
        now += 500_000_000L
        s.reference(HrMeasurement(72, listOf(830.0), true))
        val snap = s.snapshot()
        assertEquals(listOf(72f), snap.refHr.toList())
        assertEquals(2.5f, snap.refT[0], 1e-3f)
        assertEquals(72, snap.lastRef?.bpm)
        s.clear()
        assertEquals(0, s.snapshot().refT.size)
    }

    /** V5.1: breaths (@BREATH) as a curve, state (@STATE) last. */
    @Test fun atemzuegeUndZustand() {
        val s = LiveStore()
        s.feed("@IQ3_START,500,560,60.0,0", "@IQ3,1000,14,1.0,0,1.0,0,0,1572", "@STATE,1000,2,1000",
               "@BREATH,5000,300,4000,2100,15.0", "@BREATH,9100,546,4100,2200,14.8")
        val snap = s.snapshot()
        assertEquals(listOf(15.0f, 14.8f), snap.breathRate.toList())
        assertEquals(2, snap.breathT.size)
        assertEquals(2, snap.lastState?.state)
        assertEquals(2200f, snap.lastBreath?.depthUm)
    }
    @Test fun handyWerteNebenEsp32() {
        val s = LiveStore()
        s.feed(
            "@IQ3_START,500,560,60.0,0",
            "@ROOM,12211,13000,58,56,0",
            "@VITAL,12211,22.3,64.2,64.2,69.2,72.0,833.3,118.2,100,2897.9,215.2,10.5,84.95,718,3",
        )
        val esp = s.snapshot()
        listOf(
            "@DSP,12211,1,5,81.0,300000,0,0,55,0",
            "@VITAL,12211,22.0,66.5,66.5,69.0,70.0,860.0,100.0,81,2897.9,215.2,10.5,84.95,718,3",
            "@STATE,12211,3,5000",
        ).forEach { s.phone(LineParser.parse(it)) }
        s.phoneCpu(1.5f)
        val snap = s.snapshot()
        assertEquals(1, snap.phoneT.size)
        assertEquals(esp.vitalT[0], snap.phoneT[0], 1e-6f) // same point in time as the ESP32 value
        assertEquals(66.5f, snap.phoneHr[0], 1e-6f)
        assertEquals(81f, snap.phoneConf[0], 1e-6f)
        assertEquals(64.2f, snap.vitalHr[0], 1e-6f) // ESP32 series unchanged
        assertEquals(81.0f, snap.lastPhoneDsp?.confidencePct)
        assertEquals(3, snap.lastPhoneState?.state)
        assertEquals(66.5f, snap.lastPhoneVital?.hrFusedBpm)
        assertEquals(1.5f, snap.phoneCpuPct)
        s.clear()
        val c = s.snapshot()
        assertEquals(0, c.phoneT.size)
        assertEquals(null, c.lastPhoneVital)
        assertEquals(null, c.phoneCpuPct)
    }

    @Test fun handyWertZeitAusTms() {
        // Phone line with a different t_ms than the last @ROOM line: time relative to it (same time axis as vitalT)
        val s = LiveStore()
        s.feed(
            "@IQ3_START,500,560,60.0,0",
            "@ROOM,12211,13000,58,56,0",
            "@VITAL,12211,22.3,64.2,64.2,69.2,72.0,833.3,118.2,100,2897.9,215.2,10.5,84.95,718,3",
        )
        s.phone(LineParser.parse("@VITAL,13211,22.0,66.5,66.5,69.0,70.0,860.0,100.0,81,2897.9,215.2,10.5,84.95,718,3"))
        val snap = s.snapshot()
        assertEquals(snap.vitalT[0] + 1f, snap.phoneT[0], 1e-3f)
    }
}
