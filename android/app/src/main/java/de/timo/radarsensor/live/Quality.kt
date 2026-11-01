package de.timo.radarsensor.live

import de.timo.radarsensor.protocol.Dsp
import de.timo.radarsensor.protocol.MeasState
import de.timo.radarsensor.protocol.Vital
import kotlin.math.abs

/** Gütezustand des Pulses für die Anzeige (V5.1). grey: Wert nur grau zeigen; reason: warum. */
data class PulseQuality(
    val grey: Boolean,
    val stateText: String,
    val reason: String?,
)

const val MIN_CONF_PCT = 50f
const val MAX_MOTION_PCT = 30f

/**
 * Puls grau bei Konfidenz unter 50 %, Bewegung (Zustand BEWEGUNG oder > 30 % bewegte Blöcke) oder ohne gültige
 * Messung (keine Person, Suche, Erfassung). Zustandstext aus `@STATE` (V3.5), bei älterer Firmware aus der
 * Gültigkeit des Reports.
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
            st == null && d?.valid == false -> "ungültig"
            v.confidencePct < MIN_CONF_PCT -> "Konfidenz ${"%.0f".format(java.util.Locale.ROOT, v.confidencePct)} %"
            else -> null
        }
    return PulseQuality(reason != null, text, reason)
}

/** Abweichung Radar − Gurt (V5.1): letzte und Median |Δ| über windowS, n Paare. */
data class Deviation(
    val last: Float,
    val medianAbs: Float,
    val n: Int,
)

/**
 * Je Radarwert (> 0) der nächste Gurtwert innerhalb ±2 s; über die letzten windowS Sekunden (ab dem letzten
 * Radarwert). null ohne Paar.
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
