package de.timo.radarsensor.protocol

import java.util.Base64

/** Wandelt eine Zeile vom ESP32 in eine [EspMsg] um. Wirft nie; unlesbare `@`-Zeilen werden zu [Malformed]. */
object LineParser {
    fun parse(line: String): EspMsg {
        if (line.startsWith("FEHLER")) return EspError(line)
        if (!line.startsWith("@")) return Text(line)
        val p = line.split(',')
        val tag = p[0]
        return try {
            parseTag(tag, p, line)
        } catch (e: NumberFormatException) {
            Malformed(tag, line, "Zahl: ${e.message}")
        } catch (e: IndexOutOfBoundsException) {
            Malformed(tag, line, "zu wenige Felder (${p.size})")
        } catch (e: IllegalArgumentException) {
            Malformed(tag, line, e.message ?: "ungültig")
        }
    }

    private fun parseTag(
        tag: String,
        p: List<String>,
        line: String,
    ): EspMsg =
        when (tag) {
            "@IQ3" ->
                Iq3(
                    tMs = p[1].toLong(),
                    tempC = p[2].toInt(),
                    loopbackAmp = p[3].toFloat(),
                    displacementUm = p[4].toFloat(),
                    zoomAmp = p[5].toFloat(),
                    saturated = p[6] != "0",
                    frame = p.getOrNull(7)?.toLong(),
                    tick = p.getOrNull(8)?.toLong(),
                    delayed = p.getOrNull(9)?.let { it != "0" },
                )
            "@RAW" -> parseRaw(p)
            "@ROOMPROF" -> {
                val n = p[5].toInt()
                require(p.size >= 6 + 2 * n) { "@ROOMPROF: ${p.size - 6} statt ${2 * n} Werte" }
                RoomProfile(
                    tMs = p[1].toLong(),
                    tick = p[2].toLong(),
                    startMm = p[3].toFloat(),
                    stepMm = p[4].toFloat(),
                    amp = FloatArray(n) { p[6 + it].toFloat() },
                    motion = FloatArray(n) { p[6 + n + it].toFloat() },
                )
            }
            "@ROOM" -> {
                val n = p[5].toInt()
                require(p.size >= 6 + 4 * n) { "@ROOM: ${p.size - 6} statt ${4 * n} Werte" }
                Room(
                    tMs = p[1].toLong(),
                    tick = p[2].toLong(),
                    noiseAmp = p[3].toFloat(),
                    noiseMotion = p[4].toFloat(),
                    targets =
                        List(n) {
                            val b = 6 + 4 * it
                            RoomTarget(p[b].toFloat(), p[b + 1].toFloat(), p[b + 2].toFloat(), p[b + 3] != "0")
                        },
                )
            }
            "@VITAL" ->
                Vital(
                    tMs = p[1].toLong(),
                    breathingBpm = p[2].toFloat(),
                    hrFusedBpm = p[3].toFloat(),
                    hrSpectrumBpm = p[4].toFloat(),
                    hrAcfBpm = p[5].toFloat(),
                    hrIbiBpm = p[6].toFloat(),
                    ibiMedianMs = p[7].toFloat(),
                    rmssdMs = p[8].toFloat(),
                    confidencePct = p[9].toFloat(),
                    respDepthUm = p[10].toFloat(),
                    heartAmpUm = p[11].toFloat(),
                    zoomSnrDb = p[12].toFloat(),
                    loopbackPhaseStdMrad = p[13].toFloat(),
                    targetDistMm = p[14].toFloat(),
                    roomTargets = p[15].toInt(),
                )
            "@DSP" ->
                Dsp(
                    tMs = p[1].toLong(),
                    valid = p[2] != "0",
                    hrBin = p[3].toInt(),
                    confidencePct = p[4].toFloat(),
                    computeUs = p[5].toLong(),
                    motionPct = p.getOrNull(6)?.toFloat() ?: 0f,
                    hrOnHarmonic = p.getOrNull(7)?.let { it != "0" } ?: false,
                    pointAgreePct = p.getOrNull(8)?.toFloatOrNull(),
                    brState = p.getOrNull(9)?.toIntOrNull(),
                )
            "@IQ3_START" ->
                Iq3Start(p[1].toFloat(), p[2].toFloat(), p[3].toFloat(), p.getOrNull(4)?.let { it != "0" } ?: false)
            "@REZOOM" -> Rezoom(p[1].toFloat(), p[2].toFloat())
            "@RECAL" -> Recal(p[1].toLong())
            "@RECONNECT" -> Reconnect(p[1].toLong(), p[2].toInt(), p[3] != "0")
            "@WARN" -> Warn(p.drop(1).joinToString(","))
            "@END" -> End
            "@HELLO" ->
                Hello(
                    p.drop(1).mapNotNull { f ->
                        val i = f.indexOf('=')
                        if (i > 0) f.substring(0, i) to f.substring(i + 1) else null
                    }.toMap(),
                )
            "@DOPPLER_START" -> DopplerStart(p[1].toFloat(), p[2].toFloat(), p[3].toInt())
            "@DOPPLER" ->
                Doppler(
                    tMs = p[1].toLong(),
                    vRmsMmS = p[2].toFloat(),
                    amp = p[3].toFloat(),
                    tempC = p[4].toInt(),
                    frame = p.getOrNull(5)?.toLong(),
                    tick = p.getOrNull(6)?.toLong(),
                )
            "@SESS_META" ->
                SessMeta(
                    framePoints = p[1].toInt(),
                    sweepPoints = p[2].toInt(),
                    maxSweepRateHz = p[3].toFloat(),
                    highSpeed = p[4] != "0",
                    calTempC = p[5].toInt(),
                    baseStepMm = p[6].toFloat(),
                    frameKb = p[7].toFloat(),
                )
            "@SESS_SUB" -> SessSub(p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toFloat(), p[5].toFloat())
            "@PEAK" -> SessPeak(p[1].toLong(), p[2].toLong(), p[3].toInt(), p[4].toInt(), peaks(p, 5))
            "@AMP" -> SessAmp(p[1].toLong(), p[2].toInt(), p[3].toFloat(), p[4].toFloat(), FloatArray(p.size - 5) { p[5 + it].toFloat() })
            "@SESS_STAT" ->
                SessStat(
                    tMs = p[1].toLong(),
                    fps = p[2].toFloat(),
                    frames = p[3].toLong(),
                    delayed = p[4].toLong(),
                    saturated = p[5].toLong(),
                    tempC = p[6].toInt(),
                    backlog = p[7].toInt(),
                    peaks = peaks(p, 8),
                )
            "@SESS_END" -> SessEnd(p[1].toLong(), p[2].toFloat(), p[3].toFloat(), p.drop(4).map { it.toLong() })
            else -> UnknownTag(tag, line)
        }

    private fun peaks(
        p: List<String>,
        from: Int,
    ): List<SubPeak> = (from until p.size - 1 step 2).map { SubPeak(p[it].toFloat(), p[it + 1].toFloat()) }

    private fun parseRaw(p: List<String>): Raw {
        val n = p[3].toInt()
        val bytes = Base64.getDecoder().decode(p[4])
        require(bytes.size == 4 * n) { "@RAW: ${bytes.size} Bytes statt ${4 * n}" }
        val iq = ShortArray(2 * n) { i -> ((bytes[2 * i].toInt() and 0xFF) or (bytes[2 * i + 1].toInt() shl 8)).toShort() }
        return Raw(frame = p[1].toLong(), tick = p[2].toLong(), iq = iq)
    }
}
