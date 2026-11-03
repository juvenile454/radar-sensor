package de.timo.radarsensor.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import de.timo.radarsensor.core.LinkStats
import de.timo.radarsensor.core.RadarController
import de.timo.radarsensor.live.EventKind
import de.timo.radarsensor.live.LiveSnapshot
import de.timo.radarsensor.live.deviation
import de.timo.radarsensor.live.pulseQuality
import de.timo.radarsensor.protocol.Dsp
import de.timo.radarsensor.record.MetronomeSchedule
import de.timo.radarsensor.record.Recorder
import de.timo.radarsensor.reference.RefState
import de.timo.radarsensor.transport.ReplayTransport
import java.io.File

private fun LiveSnapshot.markers(): List<Marker> =
    events.mapNotNull {
        when (it.kind) {
            EventKind.REZOOM -> Marker(it.tS, Pal.warn)
            EventKind.RECONNECT -> Marker(it.tS, Pal.bad)
            EventKind.WARN, EventKind.ERROR -> Marker(it.tS, Pal.c4)
            EventKind.BREATH -> Marker(it.tS, Pal.c1)
            else -> null
        }
    }

@Composable
private fun Page(content: @Composable () -> Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 8.dp, vertical = 4.dp)) { content() }
}

// ------------------------------------------------------------------------------------------------- Overview

@Composable
fun OverviewPage(
    s: LiveSnapshot,
    stats: LinkStats,
) = Page {
    val v = s.lastVital
    val t = s.room?.targets?.firstOrNull()
    val q = pulseQuality(v, s.lastDsp, s.lastState)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        Stat("Atmung", f1(v?.breathingBpm?.takeIf { it > 0 }) + " /min", color = Pal.c1)
        Stat(
            "Puls-Kandidat",
            f0(v?.hrFusedBpm?.takeIf { it > 0 }) + " /min",
            color = if (q.grey) Pal.axis else Pal.c2,
            sub = "Konfidenz ${f0(v?.confidencePct)} % · ${q.stateText}" +
                (s.lastRef?.let { " · Gurt ${it.bpm}" } ?: " · ohne Referenz"),
        )
        Stat("Ziel", if (t != null) "%.2f m".format(t.distMm / 1000f) else "–", sub = t?.let { if (it.moving) "bewegt" else "ruhig" })
        Stat("Atemtiefe", f0(v?.respDepthUm) + " µm")
        Stat("Temperatur", s.lastIq3?.tempC?.let { "$it °C" } ?: s.lastDoppler?.tempC?.let { "$it °C" } ?: "–")
        Stat("Modus", s.mode.label, sub = "%.1f kB/s".format(stats.bytesPerS / 1000f))
        Stat(
            "Frameverlust",
            "${s.framesLost}",
            color = if (s.framesLost == 0L) Pal.ok else Pal.bad,
            sub = "${s.reconnects} × neu verbunden",
        )
    }
    LineChart("Weg (Zoom-Phase)", listOf(Series(s.iqT, s.iqDisp, Pal.c1, "µm")), xWindow = 20f, unit = "µm", markers = s.markers())
    LineChart(
        "Atmung / Puls-Kandidat",
        listOf(Series(s.vitalT, s.vitalBr, Pal.c1, "Atmung"), Series(s.vitalT, s.vitalHr, Pal.c2, "Puls-Kandidat")),
        xWindow = 300f,
        unit = "/min",
        skipNonPositive = true,
    )
    val h = s.hello
    Text(
        if (h != null) "Firmware ${h.firmware} · Server ${h.server} · Protokoll ${h.proto} · ${h.mac}" else "kein @HELLO (alte Firmware oder noch nicht verbunden)",
        fontSize = 11.sp,
        color = Pal.axis,
    )
}

// ------------------------------------------------------------------------------------------------- Signal

@Composable
fun SignalPage(s: LiveSnapshot) =
    Page {
        var window by rememberSaveable { mutableIntStateOf(20) }
        Row { listOf(5, 20, 60).forEach { w -> Quick("$w s", if (window == w) Pal.c2 else Pal.c1) { window = w } } }
        val m = s.markers()
        LineChart("Weg aus Zoom-Phase", listOf(Series(s.iqT, s.iqDisp, Pal.c1, "Weg")), xWindow = window.toFloat(), unit = "µm", markers = m, height = 200)
        LineChart("Zoom-Amplitude", listOf(Series(s.iqT, s.iqZoomAmp, Pal.c3, "Zoom")), xWindow = window.toFloat(), markers = m)
        LineChart("Loopback-Amplitude", listOf(Series(s.iqT, s.iqLoopAmp, Pal.c5, "Loopback")), xWindow = window.toFloat(), markers = m)
        LineChart("Sensortemperatur", listOf(Series(s.iqT, s.iqTemp, Pal.c2, "°C")), unit = "°C", height = 110)
        Labeled("Gesättigte Frames", "${s.iqSaturatedFrames}")
        Labeled("Verspätete Frames (Server)", "${s.iqDelayedFrames}")
        Labeled("Zoom-Fenster", if (s.zoomStartMm.isNaN()) "–" else "%.0f–%.0f mm (%d × neu gewählt)".format(s.zoomStartMm, s.zoomEndMm, s.rezooms))
        val raw = s.lastRaw
        if (raw != null && raw.points >= 1 + 24 + 80) {
            val zoomN = raw.points - 1 - 80  // 24, 48 with A121_ZOOM_POINTS=48 (V2.3)
            HorizontalDivider()
            Labeled("Rohframes (@RAW)", "${s.rawFrames}, letzter Frame ${raw.frame}, ${raw.points} Punkte")
            val zoomX = FloatArray(zoomN) { s.zoomStartMm.takeIf { !it.isNaN() }?.plus(2.5f * it) ?: it.toFloat() }
            ProfileChart("Rohframe: Zoom-Amplituden", zoomX, listOf(FloatArray(zoomN) { raw.amplitude(1 + it) } to Pal.c3), listOf("|IQ|"), height = 130)
            val rp = s.roomProfile
            val roomX = FloatArray(80) { (rp?.startMm ?: 300f) + (rp?.stepMm ?: 30f) * it }
            ProfileChart("Rohframe: Raum-Amplituden", roomX, listOf(FloatArray(80) { raw.amplitude(1 + zoomN + it) } to Pal.c1), listOf("|IQ|"), height = 130)
            Labeled("Loopback |IQ|", f0(raw.amplitude(0)))
        } else {
            Text("Rohdaten: mit „iq3 ∞ raw“ starten (≈ 35 kB/s)", fontSize = 12.sp, color = Pal.axis)
        }
    }

// ------------------------------------------------------------------------------------------------- Room

@Composable
fun RoomPage(s: LiveSnapshot) =
    Page {
        val rp = s.roomProfile
        val targets = s.room?.targets ?: emptyList()
        if (rp != null) {
            val x = FloatArray(rp.amp.size) { rp.startMm + rp.stepMm * it }
            Waterfall(
                "Bewegung über Rauschen: blau 0 dB → rot ≥ 30 dB, neueste Zeile unten",
                s.waterfall,
                rp.startMm,
                rp.stepMm,
                markerMm = targets.map { it.distMm },
            )
            ProfileChart(
                "Profil",
                x,
                listOf(rp.amp to Pal.c3, rp.motion to Pal.c2),
                listOf("Amplitude", "Bewegung"),
                markersMm = targets.mapIndexed { i, t -> t.distMm to if (i == 0) Pal.ok else Pal.axis },
                band = if (s.zoomStartMm.isNaN()) null else s.zoomStartMm to s.zoomEndMm,
                logY = true,
            )
        } else {
            Text("keine Raumprofile (iq3 starten)", color = Pal.axis)
        }
        LineChart("Hauptziel-Abstand", listOf(Series(s.targetT, s.targetDist.map { it / 1000f }.toFloatArray(), Pal.ok, "m", dots = true)), unit = "m", height = 120, markers = s.markers())
        s.room?.let { r ->
            Labeled("Rauschen Amp / Bew.", "${f0(r.noiseAmp)} / ${f0(r.noiseMotion)}")
            for ((i, t) in r.targets.withIndex()) {
                Labeled("Ziel ${i + 1}", "%.2f m · Amp %.0f · Bew. %.0f%s".format(t.distMm / 1000f, t.amp, t.motion, if (t.moving) " · bewegt" else ""))
            }
        }
    }

// ------------------------------------------------------------------------------------------------- Vital

@Composable
fun VitalPage(s: LiveSnapshot) =
    Page {
        var window by rememberSaveable { mutableIntStateOf(300) }
        Row { listOf(60, 300, 1800).forEach { w -> Quick(if (w < 600) "$w s" else "${w / 60} min", if (window == w) Pal.c2 else Pal.c1) { window = w } } }
        val xw = window.toFloat()
        val v = s.lastVital
        ReferencePanel()
        // Quality state (V5.1): pulse grey at confidence < 50 %, motion or without a valid measurement
        val q = pulseQuality(v, s.lastDsp, s.lastState)
        Labeled("Zustand", q.stateText + (q.reason?.let { " · Puls grau: $it" } ?: ""))
        Labeled(
            "Puls",
            f0(v?.hrFusedBpm?.takeIf { it > 0 }) + " /min" + (if (q.grey) " (unsicher)" else "") +
                (s.lastRef?.let { " · Gurt ${it.bpm} /min" } ?: ""),
        )
        deviation(s.vitalT, s.vitalHr, s.refT, s.refHr, windowS = 60f)?.let { d ->
            Labeled("Radar − Gurt", "zuletzt %+.0f /min · Median |Δ| %.1f /min (%d Werte, 60 s)".format(d.last, d.medianAbs, d.n))
        }
        PhoneDspPanel(s)
        Text(
            if (s.lastRef != null) {
                "Gurt als Referenz: Abweichung gilt nur für diese Sitzung, keine allgemeine Genauigkeitsaussage."
            } else {
                "Puls nur als Rhythmus-Kandidat: keine Referenzmessung, keine Genauigkeitsaussage."
            },
            fontSize = 11.sp,
            color = Pal.warn,
        )
        LineChart(
            "Atmung",
            listOf(Series(s.vitalT, s.vitalBr, Pal.c1, "Spektrum"), Series(s.breathT, s.breathRate, Pal.c3, "je Atemzug", dots = true)),
            xWindow = xw,
            unit = "/min",
            skipNonPositive = true,
        )
        s.lastBreath?.let { b -> Labeled("Letzter Atemzug", "${b.intervalMs} ms · ${f0(b.depthUm)} µm · ${f1(b.ratePerMin)} /min") }
        LineChart(
            "Puls-Kandidaten",
            listOf(
                Series(s.refT, s.refHr, Pal.ok, "Gurt"),
                Series(s.vitalT, s.vitalHr, Pal.c2, "fused"),
                Series(s.phoneT, s.phoneHr, Pal.warn, "Handy"),
                Series(s.vitalT, s.vitalHrSpec, Pal.c3, "Spektrum", dots = true),
                Series(s.vitalT, s.vitalHrAcf, Pal.c4, "AKF", dots = true),
                Series(s.vitalT, s.vitalHrIbi, Pal.c5, "IBI", dots = true),
            ),
            xWindow = xw,
            unit = "/min",
            skipNonPositive = true,
            height = 200,
        )
        LineChart("Konfidenz", listOf(Series(s.vitalT, s.vitalConf, Pal.ok, "%")), xWindow = xw, yMin = 0f, yMax = 100f, unit = "%", height = 110)
        LineChart(
            "Amplituden",
            listOf(Series(s.vitalT, s.vitalResp, Pal.c1, "Atemtiefe"), Series(s.vitalT, s.vitalHeart, Pal.c2, "Herz")),
            xWindow = xw,
            unit = "µm",
            height = 130,
        )
        LineChart("Zoom-SNR", listOf(Series(s.vitalT, s.vitalSnr, Pal.c3, "dB")), xWindow = xw, unit = "dB", height = 110)
        // RMSSD from 15 s, only with ≥ 90 % valid beats (otherwise 0, not drawn); not checked against the chest strap (V1.6)
        LineChart("RMSSD (experimentell, nicht validiert)", listOf(Series(s.vitalT, s.vitalRmssd, Pal.c5, "ms")), xWindow = xw, unit = "ms", height = 110, skipNonPositive = true)
        LineChart("DSP-Rechenzeit / Bewegungsanteil", listOf(Series(s.dspT, s.dspComputeMs, Pal.c4, "ms"), Series(s.dspT, s.dspMotion, Pal.c2, "% Bew.")), xWindow = xw, height = 120)
        if (v != null) {
            Labeled("IBI-Median", "${f0(v.ibiMedianMs)} ms")
            Labeled("Loopback-Phasenrauschen", "${f1(v.loopbackPhaseStdMrad)} mrad")
            Labeled("Zielabstand / Ziele", "%.2f m / %d".format(v.targetDistMm / 1000f, v.roomTargets))
        }
        s.lastDsp?.let { d ->
            Dsp.breathStateText(d.brState)?.let { Labeled("Atemzustand", it) }
            Labeled("DSP", "gültig ${if (d.valid) "ja" else "nein"} · Punkt ${d.hrBin} · Oberwelle ${if (d.hrOnHarmonic) "ja" else "nein"}" +
                (d.pointAgreePct?.let { " · Punkte einig ${f0(it)} %" } ?: "") +
                (d.nEff?.let { " · N_eff ${f1(it)}" } ?: ""))
        }
    }

/**
 * Phone DSP live (V5.2): the same firmware header on the phone from `@RAW`, with Viterbi and subtraction of breathing harmonics
 * (A121_DSP_HEAVY). Values at the same time next to the ESP32; needs an `iq3 … raw` stream.
 */
@Composable
private fun PhoneDspPanel(s: LiveSnapshot) {
    val on by RadarController.phoneDspOn.collectAsState()
    val err = RadarController.phoneDspError
    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        if (err != null) {
            Text("Handy-DSP nicht verfügbar: $err", fontSize = 11.sp, color = Pal.warn)
        } else if (on) {
            Quick("Handy-DSP aus", Pal.bad) { RadarController.setPhoneDsp(false) }
        } else {
            Quick("Handy-DSP ein") { RadarController.setPhoneDsp(true) }
            Quick("iq3 raw starten") { RadarController.send("iq3 0 raw") }
        }
    }
    if (!on) return
    val pv = s.lastPhoneVital
    val pq = pulseQuality(pv, s.lastPhoneDsp, s.lastPhoneState)
    Labeled(
        "Handy",
        f0(pv?.hrFusedBpm?.takeIf { it > 0 }) + " /min" + (if (pq.grey) " (unsicher)" else "") +
            " · Konfidenz ${f0(pv?.confidencePct)} % · ${pq.stateText} · CPU " + (s.phoneCpuPct?.let { "%.1f %%".format(it) } ?: "–") +
            (if (s.rawFrames == 0L) " · kein @RAW" else ""),
    )
    deviation(s.phoneT, s.phoneHr, s.vitalT, s.vitalHr, windowS = 60f)?.let { d ->
        Labeled("Handy − ESP32", "zuletzt %+.1f /min · Median |Δ| %.1f /min (%d Werte, 60 s)".format(d.last, d.medianAbs, d.n))
    }
    deviation(s.phoneT, s.phoneHr, s.refT, s.refHr, windowS = 60f)?.let { d ->
        Labeled("Handy − Gurt", "zuletzt %+.0f /min · Median |Δ| %.1f /min (%d Werte, 60 s)".format(d.last, d.medianAbs, d.n))
    }
}

/**
 * ECG chest strap (V0.1), markers and metronome breathing (V0.2). Markers end up in events.csv of the running recording
 * (names as host/vital_eval.py reads them).
 */
@Composable
private fun ReferencePanel() {
    val ref = RadarController.hrRef
    val st by ref.state.collectAsState()
    val found by ref.found.collectAsState()
    val rate by RadarController.metronome.rate.collectAsState()
    var hold by rememberSaveable { mutableStateOf(false) }
    var move by rememberSaveable { mutableStateOf(false) }
    var count by rememberSaveable { mutableStateOf(false) }
    Labeled(
        "EKG-Gurt",
        when (val x = st) {
            RefState.Idle -> "nicht verbunden"
            RefState.Scanning -> "suche …"
            is RefState.Connecting -> "verbinde ${x.address} …"
            is RefState.Connected -> "${x.name ?: x.address} · ${ref.last?.bpm ?: "–"} /min"
            is RefState.Failed -> x.message
        },
    )
    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        if (st is RefState.Connected || st is RefState.Connecting) {
            Quick("Gurt trennen", Pal.bad) { ref.disconnect() }
        } else {
            Quick("Gurt suchen") { ref.scan() }
            for (d in found) Quick("${d.name ?: d.address} (${d.rssi} dBm)", Pal.ok) { ref.connect(d.address) }
        }
    }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        Quick(if (hold) "Atem anhalten: Ende" else "Atem anhalten", if (hold) Pal.bad else Pal.c1) {
            RadarController.mark(if (hold) "anhalten_ende" else "anhalten_start")
            hold = !hold
        }
        Quick(if (move) "Bewegung: Ende" else "Bewegung", if (move) Pal.bad else Pal.c1) {
            RadarController.mark(if (move) "bewegung_ende" else "bewegung_start")
            move = !move
        }
        Quick(if (count) "Zählfenster: Ende" else "Zählfenster", if (count) Pal.bad else Pal.c1) {
            RadarController.mark(if (count) "zaehlen_ende" else "zaehlen_start")
            count = !count
        }
    }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        for (r in MetronomeSchedule.RATES) {
            Quick("$r /min", if (rate == r) Pal.c2 else Pal.c1) { RadarController.metronome.start(r) }
        }
        if (rate != null) Quick("Metronom aus", Pal.bad) { RadarController.metronome.stop() }
    }
}

// ------------------------------------------------------------------------------------------------- Doppler

@Composable
fun DopplerPage(s: LiveSnapshot) =
    Page {
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Quick("doppler 15 s") { RadarController.send("doppler 15") }
            Quick("doppler ∞") { RadarController.send("doppler 0") }
            Quick("Stopp", Pal.bad) { RadarController.stopStream() }
        }
        s.dopplerStart?.let { Labeled("Einstellung", "%.0f mm · %.0f Hz · %d Sweeps".format(it.distMm, it.sweepRateHz, it.sweeps)) }
        LineChart("v_rms", listOf(Series(s.dopT, s.dopV, Pal.c1, "mm/s")), xWindow = 30f, unit = "mm/s", height = 200, markers = s.markers())
        LineChart("Amplitude", listOf(Series(s.dopT, s.dopAmp, Pal.c3, "Amp")), xWindow = 30f, height = 140)
        s.lastDoppler?.let { Labeled("Letzter Wert", "%.2f mm/s · Amp %.0f · %d °C · Frame %s".format(it.vRmsMmS, it.amp, it.tempC, it.frame?.toString() ?: "–")) }
    }

// ------------------------------------------------------------------------------------------------- Expert

@Composable
fun ExpertPage(s: LiveSnapshot) =
    Page {
        var secs by rememberSaveable { mutableStateOf("10") }
        Text("Sparse-IQ-Session wie mit „sess …“ in der Konsole einstellen; hier starten und auswerten.", fontSize = 12.sp, color = Pal.axis)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            OutlinedTextField(secs, { secs = it.filter(Char::isDigit).take(4) }, Modifier.padding(end = 4.dp).fillMaxWidth(0.25f), label = { Text("Sek.") }, singleLine = true)
            Quick("stat") { RadarController.send("sess run $secs stat") }
            Quick("peak") { RadarController.send("sess run $secs peak") }
            Quick("amp") { RadarController.send("sess run $secs amp") }
            Quick("Stopp", Pal.bad) { RadarController.stopStream() }
            Quick("sess (zeigen)") { RadarController.send("sess") }
            Quick("sess check") { RadarController.send("sess check") }
            Quick("sess default") { RadarController.send("sess default") }
        }
        s.sessMeta?.let {
            Labeled("Frame", "${it.framePoints} Punkte · ${it.sweepPoints}/Sweep · %.1f kB".format(it.frameKb))
            Labeled("max. Sweep-Rate", "%.0f Hz · High-Speed ${if (it.highSpeed) "ja" else "nein"} · Kal. ${it.calTempC} °C".format(it.maxSweepRateHz))
        }
        for (sub in s.sessSubs) Labeled("Subsweep ${sub.index}", "Offset ${sub.offset}, ${sub.length} Punkte, ab %.0f mm, Schritt %.1f mm".format(sub.startMm, sub.stepMm))
        for ((i, a) in s.sessAmp.toSortedMap()) {
            ProfileChart("Amplitude Subsweep $i", FloatArray(a.amp.size) { a.startMm + a.stepMm * it }, listOf(a.amp to Pal.c3), listOf("Amp"), height = 140)
        }
        if (s.sessPeakT.isNotEmpty()) {
            LineChart("Stärkster Punkt (Subsweep 0)", listOf(Series(s.sessPeakT, s.sessPeakMm, Pal.c1, "mm")), xWindow = 30f, unit = "mm", height = 140)
            LineChart("Peak-Amplitude", listOf(Series(s.sessPeakT, s.sessPeakAmp, Pal.c2, "Amp")), xWindow = 30f, height = 120)
        }
        s.sessStat?.let { st ->
            Labeled("Stat", "%.1f fps · %d Frames · verzögert %d · gesättigt %d · %d °C · Rückstau %d".format(st.fps, st.frames, st.delayed, st.saturated, st.tempC, st.backlog))
            for ((i, p) in st.peaks.withIndex()) Labeled("  Sub $i Peak", "%.0f mm · Amp %.0f".format(p.distMm, p.amp))
        }
        s.sessEnd?.let { Labeled("Ende", "${it.frames} Frames · %.1f s · %.1f fps · Zähler ${it.counters.joinToString(" ")}".format(it.durationS, it.fps)) }
    }

// ------------------------------------------------------------------------------------------------- Diagnostics

@Composable
fun DiagPage(
    s: LiveSnapshot,
    stats: LinkStats,
) = Page {
    Labeled("Datenrate", "%.1f kB/s · %.0f Zeilen/s".format(stats.bytesPerS / 1000f, stats.linesPerS))
    Labeled("Empfangen", "%.2f MB · %d Zeilen".format(stats.totalBytes / 1e6, s.lines))
    Labeled("Frameverlust (Zähler)", "${s.framesLost} Frames in ${s.frameGaps} Lücken")
    Labeled("Neu verbunden", "${s.reconnects} × · Frame-Timeouts ${s.frameTimeouts}")
    Labeled("Unlesbare Zeilen", "${s.malformed}")
    s.lastMalformed?.let { Text(it, fontSize = 11.sp, color = Pal.bad, fontFamily = FontFamily.Monospace) }
    LineChart("DSP-Rechenzeit", listOf(Series(s.dspT, s.dspComputeMs, Pal.c4, "ms")), unit = "ms", height = 120)
    LineChart("Temperatur", listOf(Series(s.iqT, s.iqTemp, Pal.c2, "°C")), unit = "°C", height = 110)
    HorizontalDivider()
    Text("Zeilen je Tag", fontSize = 13.sp)
    for ((tag, n) in s.tagCounts.entries.sortedByDescending { it.value }) Labeled(tag, "$n")
    HorizontalDivider()
    Text("Ereignisse", fontSize = 13.sp)
    for (e in s.events.takeLast(40).asReversed()) {
        val c =
            when (e.kind) {
                EventKind.ERROR, EventKind.RECONNECT -> Pal.bad
                EventKind.WARN, EventKind.REZOOM -> Pal.warn
                EventKind.BREATH -> Pal.c1
                else -> Pal.axis
            }
        Text("%7.1f s  %s".format(e.tS, e.text), fontSize = 11.sp, color = c, fontFamily = FontFamily.Monospace)
    }
}

// ------------------------------------------------------------------------------------------------- Console

@Composable
fun ConsolePage(s: LiveSnapshot) {
    var cmd by rememberSaveable { mutableStateOf("") }
    val list = rememberLazyListState()
    LaunchedEffect(s.console.size, s.lines) { if (s.console.isNotEmpty()) list.scrollToItem(s.console.size - 1) }
    Column(Modifier.fillMaxSize().padding(horizontal = 8.dp)) {
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for (c in listOf("hello", "help", "info", "bus", "srv_info", "sess help")) Quick(c) { RadarController.send(c) }
            Quick("Server-Neustart-Test", Pal.warn) { RadarController.testServerReset() }
            Quick("leeren", Pal.axis) { RadarController.store.clear() }
        }
        Row {
            OutlinedTextField(
                cmd,
                { cmd = it },
                Modifier.weight(1f),
                placeholder = { Text("Befehl an den ESP32") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions =
                    KeyboardActions(onSend = {
                        RadarController.send(cmd.trim())
                        cmd = ""
                    }),
            )
            Button(onClick = {
                RadarController.send(cmd.trim())
                cmd = ""
            }, Modifier.padding(start = 6.dp)) { Text("Senden") }
        }
        LazyColumn(Modifier.fillMaxSize(), state = list) {
            items(s.console) { line ->
                Text(
                    line,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                    color =
                        when {
                            line.startsWith("FEHLER") -> Pal.bad
                            line.startsWith("@WARN") || line.startsWith("@RECONNECT") -> Pal.warn
                            line.startsWith("»") -> Pal.c1
                            line.startsWith("@") -> Pal.c3
                            else -> Color.Unspecified
                        },
                )
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------- Recordings

@Composable
fun RecordingsPage(recording: File?) =
    Page {
        val root = RadarController.recordingsDir
        var refresh by remember { mutableIntStateOf(0) }
        val dirs = remember(refresh, recording) { Recorder.list(root) }
        Text("Ablage: ${root.absolutePath}", fontSize = 11.sp, color = Pal.axis)
        Text("Holen: adb pull …/recordings ~/Data/radar-sensor/phone/", fontSize = 11.sp, color = Pal.axis)
        Row { Quick("aktualisieren") { refresh++ } }
        val dsp by RadarController.dspStatus.collectAsState()
        dsp?.let { Text("DSP am Handy: $it", fontSize = 12.sp) }
        if (dirs.isEmpty()) Text("keine Aufnahmen", color = Pal.axis)
        for (d in dirs) {
            val mb = (File(d, "stream.log").length() / 1e6)
            Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                Text(d.name, fontSize = 13.sp, color = if (d == recording) Pal.bad else Color.Unspecified)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("%.2f MB".format(mb), fontSize = 12.sp, color = Pal.axis, modifier = Modifier.padding(top = 8.dp))
                    if (d != recording) {
                        Quick("▶ ×1") { play(d, 1f) }
                        Quick("▶ ×4") { play(d, 4f) }
                        Quick("▶ ×20") { play(d, 20f) }
                        Quick("DSP neu") { RadarController.replayDsp(d) }
                    }
                }
            }
            HorizontalDivider()
        }
    }

private fun play(
    dir: File,
    speed: Float,
) {
    RadarController.store.clear()
    RadarController.connect(ReplayTransport(dir, speed))
}
