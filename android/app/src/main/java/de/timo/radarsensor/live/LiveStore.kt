package de.timo.radarsensor.live

import de.timo.radarsensor.reference.HrMeasurement
import de.timo.radarsensor.protocol.Breath
import de.timo.radarsensor.protocol.Doppler
import de.timo.radarsensor.protocol.DopplerStart
import de.timo.radarsensor.protocol.Dsp
import de.timo.radarsensor.protocol.End
import de.timo.radarsensor.protocol.EspError
import de.timo.radarsensor.protocol.EspMsg
import de.timo.radarsensor.protocol.Hello
import de.timo.radarsensor.protocol.Iq3
import de.timo.radarsensor.protocol.Iq3Start
import de.timo.radarsensor.protocol.Malformed
import de.timo.radarsensor.protocol.MeasState
import de.timo.radarsensor.protocol.Raw
import de.timo.radarsensor.protocol.Recal
import de.timo.radarsensor.protocol.Reconnect
import de.timo.radarsensor.protocol.Rezoom
import de.timo.radarsensor.protocol.Room
import de.timo.radarsensor.protocol.RoomProfile
import de.timo.radarsensor.protocol.SessAmp
import de.timo.radarsensor.protocol.SessEnd
import de.timo.radarsensor.protocol.SessMeta
import de.timo.radarsensor.protocol.SessPeak
import de.timo.radarsensor.protocol.SessStat
import de.timo.radarsensor.protocol.SessSub
import de.timo.radarsensor.protocol.Text
import de.timo.radarsensor.protocol.UnknownTag
import de.timo.radarsensor.protocol.Vital
import de.timo.radarsensor.protocol.Warn
import kotlin.math.log10
import kotlin.math.max

enum class StreamMode(
    val label: String,
) {
    IDLE("bereit"),
    IQ3("iq3"),
    DOPPLER("doppler"),
    SESS("sess"),
}

enum class EventKind { START, REZOOM, RECONNECT, WARN, ERROR, END, INFO, BREATH }

data class StreamEvent(
    val tS: Float,
    val kind: EventKind,
    val text: String,
)

/** Copy of all display data for the UI (immutable). */
class LiveSnapshot(
    val version: Long,
    val mode: StreamMode,
    val hello: Hello?,
    // @IQ3, 60 Hz
    val iqT: FloatArray,
    val iqDisp: FloatArray,
    val iqLoopAmp: FloatArray,
    val iqZoomAmp: FloatArray,
    val iqTemp: FloatArray,
    val iqSaturatedFrames: Long,
    /** Frames with frame_delayed (V1.5) */
    val iqDelayedFrames: Long,
    val lastIq3: Iq3?,
    val lastRaw: Raw?,
    val rawFrames: Long,
    // Room
    val roomProfile: RoomProfile?,
    val waterfall: List<FloatArray>,
    val waterfallT: FloatArray,
    val room: Room?,
    val targetT: FloatArray,
    val targetDist: FloatArray,
    val zoomStartMm: Float,
    val zoomEndMm: Float,
    // Vital / DSP, 1 Hz
    val vitalT: FloatArray,
    val vitalBr: FloatArray,
    val vitalHr: FloatArray,
    val vitalHrSpec: FloatArray,
    val vitalHrAcf: FloatArray,
    val vitalHrIbi: FloatArray,
    val vitalConf: FloatArray,
    val vitalResp: FloatArray,
    val vitalHeart: FloatArray,
    val vitalSnr: FloatArray,
    val vitalRmssd: FloatArray,
    val lastVital: Vital?,
    val dspT: FloatArray,
    val dspComputeMs: FloatArray,
    val dspMotion: FloatArray,
    val lastDsp: Dsp?,
    /** Measurement state (@STATE, V3.5) and breaths (@BREATH, V3.4) */
    val lastState: MeasState?,
    val breathT: FloatArray,
    val breathRate: FloatArray,
    val lastBreath: Breath?,
    /** ECG chest strap (V0.1): pulse on the same time axis as vitalT */
    val refT: FloatArray,
    val refHr: FloatArray,
    val lastRef: HrMeasurement?,
    /** Phone DSP (V5.2, A121_DSP_HEAVY): pulse and confidence on the same time axis as vitalT, CPU share in % */
    val phoneT: FloatArray,
    val phoneHr: FloatArray,
    val phoneConf: FloatArray,
    val lastPhoneVital: Vital?,
    val lastPhoneDsp: Dsp?,
    val lastPhoneState: MeasState?,
    val phoneCpuPct: Float?,
    // Doppler
    val dopT: FloatArray,
    val dopV: FloatArray,
    val dopAmp: FloatArray,
    val dopplerStart: DopplerStart?,
    val lastDoppler: Doppler?,
    // sess
    val sessMeta: SessMeta?,
    val sessSubs: List<SessSub>,
    val sessAmp: Map<Int, SessAmp>,
    val sessPeakT: FloatArray,
    val sessPeakMm: FloatArray,
    val sessPeakAmp: FloatArray,
    val sessStat: SessStat?,
    val sessEnd: SessEnd?,
    // Diagnostics
    val events: List<StreamEvent>,
    val console: List<String>,
    val lines: Long,
    val tagCounts: Map<String, Long>,
    val malformed: Long,
    val lastMalformed: String?,
    val framesLost: Long,
    val frameGaps: Long,
    val reconnects: Int,
    val frameTimeouts: Long,
    val rezooms: Int,
)

/**
 * Holds all live data. [apply] runs on the read thread, [snapshot] in the UI; both under the same lock.
 * Time axis via [TickClock]; frame loss via the frame counter in `@IQ3`/`@DOPPLER` (loss between ESP32 and
 * phone), gaps at the sensor show up on the tick time axis.
 */
class LiveStore(
    iqSeconds: Int = 60,
    private val waterfallRows: Int = 150,
    vitalCapacity: Int = 1800,
    private val nanoTime: () -> Long = System::nanoTime,
) {
    private val lock = Any()
    private var version = 0L
    private val clock = TickClock()

    /**
     * Time axis across several streams: a new stream (restart, ESP rebooted after unplugging) starts at t_ms ≈ 0;
     * it is appended after the last point, with the real pause in between, so that the charts do not run backwards.
     */
    private var tOffset = 0f
    private var tMax = 0f
    private var lastDataNs = NO_DATA

    private fun time(
        tick: Long?,
        tMs: Long,
    ): Float {
        val v = clock.seconds(tick, tMs) + tOffset
        if (v > tMax) tMax = v
        lastDataNs = nanoTime()
        return v
    }

    private var mode = StreamMode.IDLE
    private var hello: Hello? = null

    private val iqCap = 60 * iqSeconds
    private val iqT = FloatRing(iqCap)
    private val iqDisp = FloatRing(iqCap)
    private val iqLoop = FloatRing(iqCap)
    private val iqZoom = FloatRing(iqCap)
    private val iqTemp = FloatRing(iqCap)
    private var iqSaturated = 0L
    private var iqDelayed = 0L
    private var lastIq3: Iq3? = null
    private var lastRaw: Raw? = null
    private var rawFrames = 0L
    private var lastFrame = -1L

    private var roomProfile: RoomProfile? = null
    private val waterfall = ArrayDeque<FloatArray>()
    private val waterfallT = ArrayDeque<Float>()
    private var room: Room? = null
    private val targetT = FloatRing(vitalCapacity)
    private val targetDist = FloatRing(vitalCapacity)
    private var zoomStart = Float.NaN
    private var zoomEnd = Float.NaN
    private var lastRoomT = Float.NaN
    private var lastRoomTms = 0L

    private val vT = FloatRing(vitalCapacity)
    private val vBr = FloatRing(vitalCapacity)
    private val vHr = FloatRing(vitalCapacity)
    private val vHrSpec = FloatRing(vitalCapacity)
    private val vHrAcf = FloatRing(vitalCapacity)
    private val vHrIbi = FloatRing(vitalCapacity)
    private val vConf = FloatRing(vitalCapacity)
    private val vResp = FloatRing(vitalCapacity)
    private val vHeart = FloatRing(vitalCapacity)
    private val vSnr = FloatRing(vitalCapacity)
    private val vRmssd = FloatRing(vitalCapacity)
    private var lastVital: Vital? = null
    private var lastState: MeasState? = null
    private val bT = FloatRing(vitalCapacity)
    private val bRate = FloatRing(vitalCapacity)
    private var lastBreath: Breath? = null
    private val refT = FloatRing(vitalCapacity)
    private val refHr = FloatRing(vitalCapacity)
    private var lastRef: HrMeasurement? = null
    private val phT = FloatRing(vitalCapacity)
    private val phHr = FloatRing(vitalCapacity)
    private val phConf = FloatRing(vitalCapacity)
    private var lastPhoneVital: Vital? = null
    private var lastPhoneDsp: Dsp? = null
    private var lastPhoneState: MeasState? = null
    private var phoneCpuPct: Float? = null
    private val dT = FloatRing(vitalCapacity)
    private val dCompute = FloatRing(vitalCapacity)
    private val dMotion = FloatRing(vitalCapacity)
    private var lastDsp: Dsp? = null

    private val dopCap = 32 * iqSeconds
    private val dopT = FloatRing(dopCap)
    private val dopV = FloatRing(dopCap)
    private val dopAmp = FloatRing(dopCap)
    private var dopplerStart: DopplerStart? = null
    private var lastDoppler: Doppler? = null

    private var sessMeta: SessMeta? = null
    private val sessSubs = sortedMapOf<Int, SessSub>()
    private val sessAmp = HashMap<Int, SessAmp>()
    private val peakT = FloatRing(3000)
    private val peakMm = FloatRing(3000)
    private val peakAmp = FloatRing(3000)
    private var sessStat: SessStat? = null
    private var sessEnd: SessEnd? = null

    private val events = ArrayDeque<StreamEvent>()
    private val console = ArrayDeque<String>()
    private var lines = 0L
    private val tagCounts = HashMap<String, Long>()
    private var malformed = 0L
    private var lastMalformed: String? = null
    private var framesLost = 0L
    private var frameGaps = 0L
    private var reconnects = 0
    private var frameTimeouts = 0L
    private var rezooms = 0

    /** Record one received line. */
    fun apply(
        line: String,
        msg: EspMsg,
    ) = synchronized(lock) {
        lines++
        val tag = if (line.startsWith("@")) line.substringBefore(',') else if (msg is EspError) "FEHLER" else "(Text)"
        tagCounts[tag] = (tagCounts[tag] ?: 0L) + 1
        if (tag !in HIGH_RATE_TAGS) addConsole(line)
        when (msg) {
            is Iq3 -> onIq3(msg)
            is Raw -> {
                lastRaw = msg
                rawFrames++
            }
            is RoomProfile -> onRoomProfile(msg)
            is Room -> {
                room = msg
                lastRoomT = time(msg.tick, msg.tMs)
                lastRoomTms = msg.tMs
                msg.targets.firstOrNull()?.let {
                    targetT.add(lastRoomT)
                    targetDist.add(it.distMm)
                }
            }
            is Vital -> onVital(msg)
            is MeasState -> lastState = msg
            is Breath -> {
                lastBreath = msg
                bT.add(time(msg.tick, msg.tMs))
                bRate.add(msg.ratePerMin)
            }
            is Dsp -> {
                // Breathing state (V1.4): start and end of "held" into the event list
                val wasApnea = lastDsp?.brState == Dsp.BR_APNEA
                val isApnea = msg.brState == Dsp.BR_APNEA
                if (isApnea && !wasApnea) event(EventKind.BREATH, "Atem angehalten")
                if (wasApnea && !isApnea && msg.brState != null) event(EventKind.BREATH, "Atmung wieder da")
                lastDsp = msg
                dT.add(if (lastRoomT.isNaN()) time(null, msg.tMs) else lastRoomT)
                dCompute.add(msg.computeUs / 1000f)
                dMotion.add(msg.motionPct)
            }
            is Iq3Start -> {
                startStream(StreamMode.IQ3)
                zoomStart = msg.zoomStartMm
                zoomEnd = msg.zoomEndMm
                event(EventKind.START, "iq3 Zoom ${msg.zoomStartMm.toInt()}–${msg.zoomEndMm.toInt()} mm" + if (msg.raw) ", Rohdaten" else "")
            }
            is Recal -> event(EventKind.REZOOM, "Neu kalibriert")
            is Rezoom -> {
                zoomStart = msg.zoomStartMm
                zoomEnd = msg.zoomEndMm
                rezooms++
                event(EventKind.REZOOM, "Rezoom ${msg.zoomStartMm.toInt()}–${msg.zoomEndMm.toInt()} mm")
            }
            is Reconnect -> {
                if (msg.ok) reconnects++
                event(EventKind.RECONNECT, "Server neu verbunden (Versuch ${msg.attempt}, ${if (msg.ok) "ok" else "fehlgeschlagen"})")
            }
            is Warn -> {
                if (msg.what == "frame_timeout") frameTimeouts++
                event(EventKind.WARN, msg.what)
            }
            is EspError -> event(EventKind.ERROR, msg.text)
            End -> {
                event(EventKind.END, "Stream beendet")
                mode = StreamMode.IDLE
            }
            is Hello -> hello = msg
            is DopplerStart -> {
                startStream(StreamMode.DOPPLER)
                dopplerStart = msg
                event(EventKind.START, "doppler ${msg.distMm.toInt()} mm, ${msg.sweepRateHz.toInt()} Hz")
            }
            is Doppler -> onDoppler(msg)
            is SessMeta -> {
                startStream(StreamMode.SESS)
                sessMeta = msg
                sessSubs.clear()
                sessAmp.clear()
                peakT.clear()
                peakMm.clear()
                peakAmp.clear()
                sessStat = null
                sessEnd = null
                event(EventKind.START, "sess ${msg.framePoints} Punkte/Frame")
            }
            is SessSub -> sessSubs[msg.index] = msg
            is SessPeak -> {
                msg.peaks.firstOrNull()?.let {
                    peakT.add(msg.tMs / 1000f)
                    peakMm.add(it.distMm)
                    peakAmp.add(it.amp)
                }
            }
            is SessAmp -> sessAmp[msg.sub] = msg
            is SessStat -> sessStat = msg
            is SessEnd -> {
                sessEnd = msg
                mode = StreamMode.IDLE
                event(EventKind.END, "sess beendet: ${msg.frames} Frames, ${"%.1f".format(msg.fps)} Hz")
            }
            is Malformed -> {
                malformed++
                lastMalformed = "${msg.reason}: ${msg.line.take(120)}"
            }
            is Text, is UnknownTag -> Unit
        }
        version++
    }

    /** Message from the app (connection, sent command) into console and event list. */
    /**
     * Chest strap notification (V0.1): time = last radar data time + time elapsed since, so that the reference and
     * radar curves line up. Dropped without radar data (no time axis yet).
     */
    /**
     * Phone DSP (V5.2): a line recomputed on the phone (`@VITAL`, `@DSP`, `@STATE`). Its t_ms is that of the ESP32 report;
     * the time is taken relative to the last `@ROOM` line, so the phone value lies right next to the ESP32 value.
     */
    fun phone(msg: EspMsg) =
        synchronized(lock) {
            when (msg) {
                is Vital -> {
                    lastPhoneVital = msg
                    phT.add(if (lastRoomT.isNaN()) time(null, msg.tMs) else lastRoomT + (msg.tMs - lastRoomTms) / 1000f)
                    phHr.add(msg.hrFusedBpm)
                    phConf.add(msg.confidencePct)
                }
                is Dsp -> lastPhoneDsp = msg
                is MeasState -> lastPhoneState = msg
                else -> {}
            }
        }

    fun phoneCpu(pct: Float?) =
        synchronized(lock) {
            phoneCpuPct = pct
        }

    fun reference(m: HrMeasurement) {
        synchronized(lock) {
            if (lastDataNs == NO_DATA) return
            refT.add(tMax + (nanoTime() - lastDataNs) / 1e9f)
            refHr.add(m.bpm.toFloat())
            lastRef = m
            version++
        }
    }

    fun note(text: String) =
        synchronized(lock) {
            addConsole("» $text")
            event(EventKind.INFO, text)
            version++
        }

    fun version(): Long = synchronized(lock) { version }

    fun clear() =
        synchronized(lock) {
            val v = version
            val h = hello
            resetAll()
            hello = h
            version = v + 1
        }

    private fun resetAll() {
        mode = StreamMode.IDLE
        clock.reset()
        tOffset = 0f
        tMax = 0f
        lastDataNs = NO_DATA
        listOf(iqT, iqDisp, iqLoop, iqZoom, iqTemp, targetT, targetDist, vT, vBr, vHr, vHrSpec, vHrAcf, vHrIbi, vConf, vResp, vHeart, vSnr, vRmssd, bT, bRate, refT, refHr, phT, phHr, phConf, dT, dCompute, dMotion, dopT, dopV, dopAmp, peakT, peakMm, peakAmp)
            .forEach { it.clear() }
        iqSaturated = 0
        iqDelayed = 0
        lastIq3 = null
        lastRaw = null
        rawFrames = 0
        lastFrame = -1
        roomProfile = null
        waterfall.clear()
        waterfallT.clear()
        room = null
        zoomStart = Float.NaN
        zoomEnd = Float.NaN
        lastRoomT = Float.NaN
        lastVital = null
        lastDsp = null
        lastRef = null
        lastPhoneVital = null
        lastPhoneDsp = null
        lastPhoneState = null
        phoneCpuPct = null
        lastState = null
        lastBreath = null
        dopplerStart = null
        lastDoppler = null
        sessMeta = null
        sessSubs.clear()
        sessAmp.clear()
        sessStat = null
        sessEnd = null
        events.clear()
        console.clear()
        lines = 0
        tagCounts.clear()
        malformed = 0
        lastMalformed = null
        framesLost = 0
        frameGaps = 0
        reconnects = 0
        frameTimeouts = 0
        rezooms = 0
    }

    private fun startStream(m: StreamMode) {
        mode = m
        clock.reset()
        if (tMax > 0f) {
            val gapS = if (lastDataNs != NO_DATA) ((nanoTime() - lastDataNs) / 1e9f).coerceIn(0.1f, 3600f) else 1f
            tOffset = tMax + gapS
        }
        lastFrame = -1
        lastRoomT = Float.NaN
    }

    private fun countFrame(frame: Long?) {
        if (frame == null) return
        if (lastFrame >= 0 && frame != lastFrame + 1) {
            frameGaps++
            if (frame > lastFrame) framesLost += frame - lastFrame - 1
        }
        lastFrame = frame
    }

    private fun onIq3(m: Iq3) {
        if (mode != StreamMode.IQ3) mode = StreamMode.IQ3 // connection mid-stream
        countFrame(m.frame)
        val t = time(m.tick, m.tMs)
        iqT.add(t)
        iqDisp.add(m.displacementUm)
        iqLoop.add(m.loopbackAmp)
        iqZoom.add(m.zoomAmp)
        iqTemp.add(m.tempC.toFloat())
        if (m.saturated) iqSaturated++
        if (m.delayed == true) iqDelayed++
        lastIq3 = m
    }

    private fun onRoomProfile(m: RoomProfile) {
        roomProfile = m
        val n = m.motion.size
        if (n == 0) return
        // motion over noise in dB, noise = lower quarter of the points (as in firmware and PC evaluation)
        val noise = m.motion.sortedArray()[n / 4] + 1f
        val row = FloatArray(n) { 20f * log10(max(m.motion[it] / noise, 1e-3f)) }
        waterfall.addLast(row)
        waterfallT.addLast(time(m.tick, m.tMs))
        while (waterfall.size > waterfallRows) {
            waterfall.removeFirst()
            waterfallT.removeFirst()
        }
    }

    private fun onVital(m: Vital) {
        lastVital = m
        vT.add(if (lastRoomT.isNaN()) time(null, m.tMs) else lastRoomT)
        vBr.add(m.breathingBpm)
        vHr.add(m.hrFusedBpm)
        vHrSpec.add(m.hrSpectrumBpm)
        vHrAcf.add(m.hrAcfBpm)
        vHrIbi.add(m.hrIbiBpm)
        vConf.add(m.confidencePct)
        vResp.add(m.respDepthUm)
        vHeart.add(m.heartAmpUm)
        vSnr.add(m.zoomSnrDb)
        vRmssd.add(m.rmssdMs)
    }

    private fun onDoppler(m: Doppler) {
        if (mode != StreamMode.DOPPLER) mode = StreamMode.DOPPLER
        countFrame(m.frame)
        dopT.add(time(m.tick, m.tMs))
        dopV.add(m.vRmsMmS)
        dopAmp.add(m.amp)
        lastDoppler = m
    }

    private fun addConsole(s: String) {
        console.addLast(s)
        while (console.size > CONSOLE_LINES) console.removeFirst()
    }

    private fun event(
        kind: EventKind,
        text: String,
    ) {
        events.addLast(StreamEvent(tMax, kind, text))
        while (events.size > 200) events.removeFirst()
    }

    fun snapshot(): LiveSnapshot =
        synchronized(lock) {
            LiveSnapshot(
                version = version,
                mode = mode,
                hello = hello,
                iqT = iqT.toArray(),
                iqDisp = iqDisp.toArray(),
                iqLoopAmp = iqLoop.toArray(),
                iqZoomAmp = iqZoom.toArray(),
                iqTemp = iqTemp.toArray(),
                iqSaturatedFrames = iqSaturated,
                iqDelayedFrames = iqDelayed,
                lastIq3 = lastIq3,
                lastRaw = lastRaw,
                rawFrames = rawFrames,
                roomProfile = roomProfile,
                waterfall = waterfall.toList(),
                waterfallT = waterfallT.toFloatArray(),
                room = room,
                targetT = targetT.toArray(),
                targetDist = targetDist.toArray(),
                zoomStartMm = zoomStart,
                zoomEndMm = zoomEnd,
                vitalT = vT.toArray(),
                vitalBr = vBr.toArray(),
                vitalHr = vHr.toArray(),
                vitalHrSpec = vHrSpec.toArray(),
                vitalHrAcf = vHrAcf.toArray(),
                vitalHrIbi = vHrIbi.toArray(),
                vitalConf = vConf.toArray(),
                vitalResp = vResp.toArray(),
                vitalHeart = vHeart.toArray(),
                vitalSnr = vSnr.toArray(),
                vitalRmssd = vRmssd.toArray(),
                lastVital = lastVital,
                dspT = dT.toArray(),
                dspComputeMs = dCompute.toArray(),
                dspMotion = dMotion.toArray(),
                lastDsp = lastDsp,
                lastState = lastState,
                breathT = bT.toArray(),
                breathRate = bRate.toArray(),
                lastBreath = lastBreath,
                refT = refT.toArray(),
                refHr = refHr.toArray(),
                lastRef = lastRef,
                phoneT = phT.toArray(),
                phoneHr = phHr.toArray(),
                phoneConf = phConf.toArray(),
                lastPhoneVital = lastPhoneVital,
                lastPhoneDsp = lastPhoneDsp,
                lastPhoneState = lastPhoneState,
                phoneCpuPct = phoneCpuPct,
                dopT = dopT.toArray(),
                dopV = dopV.toArray(),
                dopAmp = dopAmp.toArray(),
                dopplerStart = dopplerStart,
                lastDoppler = lastDoppler,
                sessMeta = sessMeta,
                sessSubs = sessSubs.values.toList(),
                sessAmp = HashMap(sessAmp),
                sessPeakT = peakT.toArray(),
                sessPeakMm = peakMm.toArray(),
                sessPeakAmp = peakAmp.toArray(),
                sessStat = sessStat,
                sessEnd = sessEnd,
                events = events.toList(),
                console = console.toList(),
                lines = lines,
                tagCounts = HashMap(tagCounts),
                malformed = malformed,
                lastMalformed = lastMalformed,
                framesLost = framesLost,
                frameGaps = frameGaps,
                reconnects = reconnects,
                frameTimeouts = frameTimeouts,
                rezooms = rezooms,
            )
        }

    companion object {
        const val CONSOLE_LINES = 400
        private const val NO_DATA = Long.MIN_VALUE

        /** Too frequent for the console (only counted and plotted). */
        val HIGH_RATE_TAGS = setOf("@IQ3", "@RAW", "@ROOMPROF", "@DOPPLER", "@PEAK", "@AMP")
    }
}
