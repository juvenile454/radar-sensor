package de.timo.radarsensor.protocol

/**
 * Eine Zeile vom ESP32-S3 (Zeilenprotokoll `@TAG,feld,…`, Firmware `firmware/esp32s3/src/main.cpp`).
 * Felder kommen nur hinten dazu; ältere Firmware ohne Frame-Zähler/tick liefert dort `null`.
 */
sealed interface EspMsg

/** `@HELLO,proto=1,fw=…,link=…,server=…,mac=…,up_ms=…,modes=…` */
data class Hello(
    val fields: Map<String, String>,
) : EspMsg {
    val proto: Int? get() = fields["proto"]?.toIntOrNull()
    val firmware: String? get() = fields["fw"]
    val server: String? get() = fields["server"]
    val mac: String? get() = fields["mac"]
    val upMs: Long? get() = fields["up_ms"]?.toLongOrNull()
}

/** `@IQ3_START,zoom_start_mm,zoom_end_mm,frame_rate[,raw]` */
data class Iq3Start(
    val zoomStartMm: Float,
    val zoomEndMm: Float,
    val frameRateHz: Float,
    val raw: Boolean,
) : EspMsg

/** `@IQ3,t_ms,temp,lb_amp,disp_um,zoom_amp,saturated[,frame,tick[,delayed]]` je Frame (60 Hz). */
data class Iq3(
    val tMs: Long,
    val tempC: Int,
    val loopbackAmp: Float,
    val displacementUm: Float,
    val zoomAmp: Float,
    val saturated: Boolean,
    val frame: Long?,
    val tick: Long?,
    /** frame_delayed vom Server (seit V1.5; ältere Firmware: null) */
    val delayed: Boolean? = null,
) : EspMsg

/**
 * `@RAW,frame,tick,n,<Base64>`: n komplexe int16-Punkte (real, imag, Little Endian) unverändert vom Server.
 * Reihenfolge bei `iq3`: 1 Loopback, 24 Zoom-Punkte (2,5 mm; mit A121_ZOOM_POINTS=48 in der Firmware 48), 80 Raum-Punkte.
 */
class Raw(
    val frame: Long,
    val tick: Long,
    /** real0, imag0, real1, imag1, … */
    val iq: ShortArray,
) : EspMsg {
    val points: Int get() = iq.size / 2

    fun amplitude(i: Int): Float {
        val re = iq[2 * i].toFloat()
        val im = iq[2 * i + 1].toFloat()
        return kotlin.math.sqrt(re * re + im * im)
    }
}

/** `@ROOMPROF,t_ms,tick,start_mm,step_mm,n,amp×n,bew×n` (5 Hz) */
class RoomProfile(
    val tMs: Long,
    val tick: Long,
    val startMm: Float,
    val stepMm: Float,
    val amp: FloatArray,
    val motion: FloatArray,
) : EspMsg

data class RoomTarget(
    val distMm: Float,
    val amp: Float,
    val motion: Float,
    val moving: Boolean,
)

/** `@ROOM,t_ms,tick,rausch_amp,rausch_bew,n,{dist_mm,amp,bew,bewegt}×n` (1 Hz, Ziele nach Stärke sortiert) */
data class Room(
    val tMs: Long,
    val tick: Long,
    val noiseAmp: Float,
    val noiseMotion: Float,
    val targets: List<RoomTarget>,
) : EspMsg

/** `@VITAL,…` (1 Hz, nur bei gültigem Report). Feldreihenfolge wie `VITAL_FIELDS` in host/a121_esp_ultimate.py. */
data class Vital(
    val tMs: Long,
    val breathingBpm: Float,
    val hrFusedBpm: Float,
    val hrSpectrumBpm: Float,
    val hrAcfBpm: Float,
    val hrIbiBpm: Float,
    val ibiMedianMs: Float,
    val rmssdMs: Float,
    val confidencePct: Float,
    val respDepthUm: Float,
    val heartAmpUm: Float,
    val zoomSnrDb: Float,
    val loopbackPhaseStdMrad: Float,
    val targetDistMm: Float,
    val roomTargets: Int,
) : EspMsg

/**
 * `@DSP,t_ms,gültig,punkt,konfidenz,rechenzeit_us,bewegung_pct,hr_harm,point_agree_pct,br_state` (1 Hz).
 * Ältere Firmware schickt weniger Felder; fehlende hinten sind null.
 */
data class Dsp(
    val tMs: Long,
    val valid: Boolean,
    val hrBin: Int,
    val confidencePct: Float,
    val computeUs: Long,
    val motionPct: Float,
    val hrOnHarmonic: Boolean,
    /** frühere Konfidenz: Anteil der Zoom-Punkte mit eigener Spitze ±5 /min (Diagnose, V1.3) */
    val pointAgreePct: Float? = null,
    /** Atemzustand (V1.4): 0 normal, 1 keine Atemspitze, 2 angehalten, 3 Bewegung, 4 flach */
    val brState: Int? = null,
) : EspMsg {
    companion object {
        const val BR_APNEA = 2

        fun breathStateText(state: Int?): String? =
            when (state) {
                null -> null
                0 -> "normal"
                1 -> "keine Atemspitze"
                2 -> "angehalten"
                3 -> "Bewegung"
                4 -> "flach"
                else -> "unbekannt ($state)"
            }
    }
}

/** `@RECAL,t_ms`: Sensor meldete calibration_needed, Session mit gleichem Zoom-Fenster neu aufgesetzt (V1.5). */
data class Recal(val tMs: Long) : EspMsg

/** `@REZOOM,start_mm,end_mm` */
data class Rezoom(
    val zoomStartMm: Float,
    val zoomEndMm: Float,
) : EspMsg

/** `@RECONNECT,t_ms,versuch,ok`: Server nach Frame-Timeouts neu verbunden (tick beginnt danach neu). */
data class Reconnect(
    val tMs: Long,
    val attempt: Int,
    val ok: Boolean,
) : EspMsg

/** `@WARN,was` */
data class Warn(
    val what: String,
) : EspMsg

/** `@END`: Stream (iq3, doppler) beendet. */
data object End : EspMsg

/** `@DOPPLER_START,dist_mm,sweep_rate_hz,sweeps` */
data class DopplerStart(
    val distMm: Float,
    val sweepRateHz: Float,
    val sweeps: Int,
) : EspMsg

/** `@DOPPLER,t_ms,v_rms_mm_s,amp,temp[,frame,tick]` (~31 Hz) */
data class Doppler(
    val tMs: Long,
    val vRmsMmS: Float,
    val amp: Float,
    val tempC: Int,
    val frame: Long?,
    val tick: Long?,
) : EspMsg

/** `@SESS_META,frame_pts,sweep_pts,max_sweep_rate,high_speed,cal_temp,base_step_mm,frame_kb` */
data class SessMeta(
    val framePoints: Int,
    val sweepPoints: Int,
    val maxSweepRateHz: Float,
    val highSpeed: Boolean,
    val calTempC: Int,
    val baseStepMm: Float,
    val frameKb: Float,
) : EspMsg

/** `@SESS_SUB,i,offset,länge,start_mm,step_mm` */
data class SessSub(
    val index: Int,
    val offset: Int,
    val length: Int,
    val startMm: Float,
    val stepMm: Float,
) : EspMsg

/** Stärkster Punkt eines Subsweeps */
data class SubPeak(
    val distMm: Float,
    val amp: Float,
)

/** `@PEAK,t_ms,tick,temp,flags,{mm,amp}×subs` (je Frame) — flags: 1 gesättigt, 2 verzögert, 4 Kalibrierung nötig */
data class SessPeak(
    val tMs: Long,
    val tick: Long,
    val tempC: Int,
    val flags: Int,
    val peaks: List<SubPeak>,
) : EspMsg

/** `@AMP,t_ms,sub,start_mm,step_mm,amp…` (10 Hz je Subsweep) */
class SessAmp(
    val tMs: Long,
    val sub: Int,
    val startMm: Float,
    val stepMm: Float,
    val amp: FloatArray,
) : EspMsg

/** `@SESS_STAT,t_ms,fps,frames,verzögert,gesättigt,temp,rückstau,{mm,amp}×subs` (1 Hz) */
data class SessStat(
    val tMs: Long,
    val fps: Float,
    val frames: Long,
    val delayed: Long,
    val saturated: Long,
    val tempC: Int,
    val backlog: Int,
    val peaks: List<SubPeak>,
) : EspMsg

/** `@SESS_END,frames,dauer_s,fps,verzögert,gesättigt,kal,timeouts,err_ovf,err_buf,err_frame,max_rückstau` */
data class SessEnd(
    val frames: Long,
    val durationS: Float,
    val fps: Float,
    val counters: List<Long>,
) : EspMsg

/** Zeile mit `FEHLER` */
data class EspError(
    val text: String,
) : EspMsg

/** Freitext (help, info, bus, srv_info, …) */
data class Text(
    val text: String,
) : EspMsg

/** `@…`-Zeile mit unbekanntem Tag (neuere Firmware) */
data class UnknownTag(
    val tag: String,
    val line: String,
) : EspMsg

/** `@…`-Zeile mit bekanntem Tag, aber nicht lesbar (z. B. abgeschnitten) */
data class Malformed(
    val tag: String,
    val line: String,
    val reason: String,
) : EspMsg
