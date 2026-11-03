package de.timo.radarsensor.live

import de.timo.radarsensor.protocol.Dsp
import de.timo.radarsensor.protocol.MeasState
import de.timo.radarsensor.protocol.Vital
import kotlin.math.abs

/** Quality state of the pulse for the display (V5.1). grey: show the value in grey only; reason: why. */
data class PulseQuality(
    val grey: Boolean,
    val stateText: String,
    val reason: String?,
)

const val MIN_CONF_PCT = 50f
const val MAX_MOTION_PCT = 30f

/** @VITAL older than the last @DSP by more than this: value comes from an earlier report (reports arrive 1/s). */
const val STALE_MS = 1500L

/**
 * Pulse grey at confidence below 50 %, motion (state BEWEGUNG or > 30 % moving blocks), without a valid
 * measurement (no person, search, acquisition), with an invalid last report (in any state, also apnea: then there is
 * no new @VITAL, the old value would be displayed) and when the value is older than the last report. State text from
 * `@STATE` (V3.5), with older firmware from the validity of the report.
 */
fun pulseQuality(
    v: Vital?,
    d: Dsp?,
    st: MeasState?,
): PulseQuality {
    val text =
        when (st?.state) {
            null -> if (d?.valid == true) "gültig" else "ungültig"
            MeasState.ACQUIRE -> "Erfassung läuft ${st.sinceMs / 1000} s"
            MeasState.VALID -> "gültig seit ${st.sinceMs / 1000} s"
            MeasState.APNEA -> "Atem angehalten (Apnoe)"
            else -> MeasState.name(st.state)
        }
    val reason =
        when {
            v == null -> "kein Wert"
            st?.state == MeasState.MOTION || (d?.motionPct ?: 0f) > MAX_MOTION_PCT -> "Bewegung"
            st != null && st.state !in setOf(MeasState.VALID, MeasState.APNEA) -> MeasState.name(st.state)
            d?.valid == false -> "ungültig"
            d != null && d.tMs - v.tMs > STALE_MS -> "veraltet"
            v.confidencePct < MIN_CONF_PCT -> "Konfidenz ${"%.0f".format(java.util.Locale.ROOT, v.confidencePct)} %"
            else -> null
        }
    return PulseQuality(reason != null, text, reason)
}

/** Deviation radar − chest strap (V5.1): last and median |Δ| over windowS, n pairs. */
data class Deviation(
    val last: Float,
    val medianAbs: Float,
    val n: Int,
)

/**
 * For each radar value (> 0) the nearest chest strap value within ±2 s; over the last windowS seconds (from the last
 * radar value). null without a pair.
 */
fun deviation(
    vT: FloatArray,
    vHr: FloatArray,
    refT: FloatArray,
    refHr: FloatArray,
    windowS: Float,
): Deviation? {
    if (vT.isEmpty() || refT.isEmpty()) return null
    val tEnd = vT.last()
    val d = ArrayList<Float>()
    for (i in vT.indices) {
        if (vT[i] < tEnd - windowS || !(vHr[i] > 0f)) continue
        var best = -1
        for (j in refT.indices) if (best < 0 || abs(refT[j] - vT[i]) < abs(refT[best] - vT[i])) best = j
        if (best >= 0 && abs(refT[best] - vT[i]) <= 2f && refHr[best] > 0f) d += vHr[i] - refHr[best]
    }
    if (d.isEmpty()) return null
    val a = d.map { abs(it) }.sorted()
    val med = if (a.size % 2 == 1) a[a.size / 2] else 0.5f * (a[a.size / 2 - 1] + a[a.size / 2])
    return Deviation(d.last(), med, d.size)
}
