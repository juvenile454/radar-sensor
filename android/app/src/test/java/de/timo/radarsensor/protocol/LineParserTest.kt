package de.timo.radarsensor.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Zeilen wörtlich aus den Testaufnahmen 2026-11-01 (Firmware 2026-11-01-app1, Server a121-v1.13.0). */
class LineParserTest {
    private inline fun <reified T : EspMsg> parse(line: String): T {
        val m = LineParser.parse(line)
        assertTrue("erwartet ${T::class.simpleName}, bekommen $m", m is T)
        return m as T
    }

    @Test fun iq3MitZaehlerUndTick() {
        val m = parse<Iq3>("@IQ3,8,14,1419.0,0.00,206.6,0,0,572")
        assertEquals(8L, m.tMs)
        assertEquals(14, m.tempC)
        assertEquals(1419.0f, m.loopbackAmp)
        assertEquals(0f, m.displacementUm)
        assertEquals(206.6f, m.zoomAmp)
        assertEquals(false, m.saturated)
        assertEquals(0L, m.frame)
        assertEquals(572L, m.tick)
    }

    @Test fun iq3MitDelayed() {
        // V1.5: frame_delayed hinten an @IQ3
        assertEquals(true, parse<Iq3>("@IQ3,8,14,1419.0,0.00,206.6,0,0,572,1").delayed)
        assertNull(parse<Iq3>("@IQ3,8,14,1419.0,0.00,206.6,0,0,572").delayed)
    }

    @Test fun neuKalibrierung() {
        assertEquals(Recal(12345L), LineParser.parse("@RECAL,12345"))
    }

    @Test fun iq3AlteFirmwareOhneZaehler() {
        val m = parse<Iq3>("@IQ3,8,14,1419.0,-3.25,206.6,1")
        assertNull(m.frame)
        assertNull(m.tick)
        assertEquals(-3.25f, m.displacementUm)
        assertEquals(true, m.saturated)
    }

    @Test fun iq3Start() {
        val m = parse<Iq3Start>("@IQ3_START,500,560,60.0,1")
        assertEquals(500f, m.zoomStartMm)
        assertEquals(560f, m.zoomEndMm)
        assertEquals(60f, m.frameRateHz)
        assertTrue(m.raw)
        assertEquals(false, parse<Iq3Start>("@IQ3_START,500,560,60.0").raw)
    }

    @Test fun raw() {
        // 2 Punkte: (1, -2), (300, -32768)
        val bytes = byteArrayOf(1, 0, -2, -1, 44, 1, 0, -128)
        val b64 = java.util.Base64.getEncoder().encodeToString(bytes)
        val m = parse<Raw>("@RAW,7,1234,2,$b64")
        assertEquals(7L, m.frame)
        assertEquals(1234L, m.tick)
        assertEquals(2, m.points)
        assertEquals(listOf<Short>(1, -2, 300, -32768), m.iq.toList())
        assertEquals(kotlin.math.sqrt(5f), m.amplitude(0), 1e-5f)
    }

    @Test fun rawFalscheLaengeIstMalformed() {
        val m = parse<Malformed>("@RAW,7,1234,3,AQD+/w==")
        assertEquals("@RAW", m.tag)
    }

    @Test fun roomProfile() {
        val amp = (1..80).joinToString(",")
        val mot = (101..180).joinToString(",")
        val m = parse<RoomProfile>("@ROOMPROF,192,755,300,30,80,$amp,$mot")
        assertEquals(755L, m.tick)
        assertEquals(300f, m.startMm)
        assertEquals(30f, m.stepMm)
        assertEquals(80, m.amp.size)
        assertEquals(1f, m.amp[0])
        assertEquals(180f, m.motion[79])
    }

    @Test fun roomProfileAbgeschnitten() {
        assertTrue(LineParser.parse("@ROOMPROF,192,755,300,30,80,1,2,3") is Malformed)
    }

    @Test fun room() {
        val m = parse<Room>("@ROOM,1010,1571,58,56,3,719,3650,4128,1,542,167,236,1,1346,117,156,0")
        assertEquals(1571L, m.tick)
        assertEquals(58f, m.noiseAmp)
        assertEquals(3, m.targets.size)
        assertEquals(RoomTarget(719f, 3650f, 4128f, true), m.targets[0])
        assertEquals(RoomTarget(1346f, 117f, 156f, false), m.targets[2])
        assertEquals(0, parse<Room>("@ROOM,1010,1571,58,56,0").targets.size)
    }

    @Test fun vital() {
        val m = parse<Vital>("@VITAL,12211,22.3,64.2,64.2,69.2,72.0,833.3,118.2,100,2897.9,215.2,10.5,84.95,718,3")
        assertEquals(12211L, m.tMs)
        assertEquals(22.3f, m.breathingBpm)
        assertEquals(64.2f, m.hrFusedBpm)
        assertEquals(69.2f, m.hrAcfBpm)
        assertEquals(72.0f, m.hrIbiBpm)
        assertEquals(833.3f, m.ibiMedianMs)
        assertEquals(118.2f, m.rmssdMs)
        assertEquals(100f, m.confidencePct)
        assertEquals(2897.9f, m.respDepthUm)
        assertEquals(215.2f, m.heartAmpUm)
        assertEquals(10.5f, m.zoomSnrDb)
        assertEquals(84.95f, m.loopbackPhaseStdMrad)
        assertEquals(718f, m.targetDistMm)
        assertEquals(3, m.roomTargets)
    }

    @Test fun dsp() {
        val m = parse<Dsp>("@DSP,1009,0,0,0.0,327,0,0")
        assertEquals(false, m.valid)
        assertEquals(327L, m.computeUs)
        val alt = parse<Dsp>("@DSP,1009,1,57,81.5,51234")
        assertEquals(true, alt.valid)
        assertEquals(57, alt.hrBin)
        assertEquals(0f, alt.motionPct)
        assertEquals(null, alt.brState)
        // V1.3/V1.4: point_agree_pct und br_state hinten angehängt
        val neu = parse<Dsp>("@DSP,1009,1,5,72.0,300000,0,0,55,2")
        assertEquals(55f, neu.pointAgreePct)
        assertEquals(2, neu.brState)
        assertEquals(null, neu.nEff)
        // V3.1: effektive Punktzahl als Feld 14, nur mit Schalter A121_DSP_COMBINE
        val komb = parse<Dsp>("@DSP,1009,1,5,72.0,300000,0,0,100,0,0,0,0,1234,1.4")
        assertEquals(1.4f, komb.nEff)
    }

    /** V6: win_s und reacq als Felder 15 und 16 (A121_DSP_REACQ); Feld 14 (n_eff) bleibt ohne A121_DSP_COMBINE leer. */
    @Test fun dspWithReacqFields() {
        val d = parse<Dsp>("@DSP,1000,1,5,88.0,300000,0,0,0,0,0,0,0,60,,7,2")
        assertEquals(7, d.winS)
        assertEquals(2, d.reacq)
        assertEquals(null, d.nEff)
    }

    @Test fun dspWithoutReacqFields() {
        val d = parse<Dsp>("@DSP,1000,1,5,88.0,300000,0,0,0,0,0,0,0,60")
        assertEquals(null, d.winS)
        assertEquals(null, d.reacq)
    }

    /** V3.5/V5.1: Zustand der Messung je Report. */
    @Test fun messZustand() {
        val m = parse<MeasState>("@STATE,12000,2,4000")
        assertEquals(12000L, m.tMs)
        assertEquals(MeasState.ACQUIRE, m.state)
        assertEquals(4000L, m.sinceMs)
        assertEquals("Erfassung", MeasState.name(2))
        assertEquals("keine Person", MeasState.name(0))
        assertEquals("Apnoe", MeasState.name(5))
        assertEquals("unbekannt (9)", MeasState.name(9))
    }

    /** V3.4/V5.1: ein Atemzug. */
    @Test fun atemzug() {
        val b = parse<Breath>("@BREATH,15000,900,4100,2300,14.6")
        assertEquals(15000L, b.tMs)
        assertEquals(900L, b.tick)
        assertEquals(4100L, b.intervalMs)
        assertEquals(2300f, b.depthUm)
        assertEquals(14.6f, b.ratePerMin)
    }

    @Test fun atemzustandText() {
        assertEquals("normal", Dsp.breathStateText(0))
        assertEquals("keine Atemspitze", Dsp.breathStateText(1))
        assertEquals("angehalten", Dsp.breathStateText(2))
        assertEquals("Bewegung", Dsp.breathStateText(3))
        assertEquals("flach", Dsp.breathStateText(4))
        assertEquals(null, Dsp.breathStateText(null))
        assertEquals("unbekannt (7)", Dsp.breathStateText(7))
    }

    @Test fun ereignisse() {
        assertEquals(Rezoom(687f, 747f), LineParser.parse("@REZOOM,687,747"))
        assertEquals(Reconnect(10967L, 1, true), LineParser.parse("@RECONNECT,10967,1,1"))
        assertEquals(Warn("test_server_reset"), LineParser.parse("@WARN,test_server_reset"))
        assertEquals(End, LineParser.parse("@END"))
        assertEquals(EspError("FEHLER: Server antwortet nicht"), LineParser.parse("FEHLER: Server antwortet nicht"))
        assertEquals(Text("  iq3 [sek] [start_mm] [raw]  3-Subsweep"), LineParser.parse("  iq3 [sek] [start_mm] [raw]  3-Subsweep"))
        assertTrue(LineParser.parse("@NEU,1,2") is UnknownTag)
        assertTrue(LineParser.parse("@IQ3,abc") is Malformed)
    }

    @Test fun hello() {
        val m =
            parse<Hello>(
                "@HELLO,proto=1,fw=2026-11-01-app1,build=Nov  1 2026 12:00:00,link=srv,server=a121-v1.13.0,mac=12:34:56:78:9A:BC,up_ms=3441,modes=iq3 doppler sess bridge i2c",
            )
        assertEquals(1, m.proto)
        assertEquals("2026-11-01-app1", m.firmware)
        assertEquals("a121-v1.13.0", m.server)
        assertEquals("12:34:56:78:9A:BC", m.mac)
        assertEquals(3441L, m.upMs)
        assertEquals("iq3 doppler sess bridge i2c", m.fields["modes"])
    }

    @Test fun doppler() {
        assertEquals(DopplerStart(500f, 2000f, 64), LineParser.parse("@DOPPLER_START,500,2000,64"))
        val m = parse<Doppler>("@DOPPLER,39,193.33,119.7,18,0,590")
        assertEquals(193.33f, m.vRmsMmS)
        assertEquals(18, m.tempC)
        assertEquals(0L, m.frame)
        assertEquals(590L, m.tick)
        assertNull(parse<Doppler>("@DOPPLER,39,193.33,119.7,18").frame)
    }

    @Test fun sess() {
        val meta = parse<SessMeta>("@SESS_META,40,40,2574.7,1,16,2.5023,0.16")
        assertEquals(40, meta.framePoints)
        assertEquals(true, meta.highSpeed)
        assertEquals(2.5023f, meta.baseStepMm)
        assertEquals(SessSub(0, 0, 40, 200.2f, 5f), LineParser.parse("@SESS_SUB,0,0,40,200.2,5.00"))
        val peak = parse<SessPeak>("@PEAK,2,560,15,0,200.2,584")
        assertEquals(560L, peak.tick)
        assertEquals(listOf(SubPeak(200.2f, 584f)), peak.peaks)
        val amp = parse<SessAmp>("@AMP,3,0,200.2,5.00,563,456,373")
        assertEquals(0, amp.sub)
        assertEquals(listOf(563f, 456f, 373f), amp.amp.toList())
        val st = parse<SessStat>("@SESS_STAT,1002,10.98,11,0,0,15,0,200.2,653")
        assertEquals(10.98f, st.fps)
        assertEquals(11L, st.frames)
        assertEquals(listOf(SubPeak(200.2f, 653f)), st.peaks)
        val end = parse<SessEnd>("@SESS_END,51,5.00,10.00,0,0,0,0,0,0,0,0")
        assertEquals(51L, end.frames)
        assertEquals(8, end.counters.size)
    }
}
