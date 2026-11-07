package io.github.juvenile454.radarsensor.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

object Pal {
    val grid = Color(0x33FFFFFF)
    val axis = Color(0x99FFFFFF)
    val c1 = Color(0xFF4FD1C5)
    val c2 = Color(0xFFF6AD55)
    val c3 = Color(0xFF90CDF4)
    val c4 = Color(0xFFF687B3)
    val c5 = Color(0xFFB794F4)
    val ok = Color(0xFF68D391)
    val warn = Color(0xFFF6E05E)
    val bad = Color(0xFFFC8181)
}

data class Series(
    val x: FloatArray,
    val y: FloatArray,
    val color: Color,
    val label: String,
    val dots: Boolean = false,
)

/** Vertical event line (rezoom, reconnect, warning) on the time axis */
data class Marker(
    val x: Float,
    val color: Color,
)

private fun niceStep(range: Float, target: Int): Float {
    if (range <= 0f || range.isNaN()) return 1f
    val raw = range / target
    val p = 10f.pow(floor(log10(raw)))
    val f = raw / p
    return p * when {
        f < 1.5f -> 1f
        f < 3.5f -> 2f
        f < 7.5f -> 5f
        else -> 10f
    }
}

private fun fmt(value: Float, step: Float): String {
    val v = if (abs(value) < step * 1e-3f) 0f else value // no "-0"
    return when {
        step >= 1f -> "%.0f".format(v)
        step >= 0.1f -> "%.1f".format(v)
        else -> "%.2f".format(v)
    }
}

/**
 * Line chart with a shared x axis. [xWindow]: show only the last seconds. y range automatic
 * (robustness against single outliers not needed: ESP values are bounded) or fixed via [yMin]/[yMax].
 * NaN and ≤ 0 with [skipNonPositive] are left out (e.g. pulse 0 = no value).
 */
@Composable
fun LineChart(
    title: String,
    series: List<Series>,
    modifier: Modifier = Modifier,
    height: Int = 160,
    xWindow: Float? = null,
    yMin: Float? = null,
    yMax: Float? = null,
    unit: String = "",
    markers: List<Marker> = emptyList(),
    skipNonPositive: Boolean = false,
) {
    Column(modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(title, style = MaterialTheme.typography.labelLarge)
            if (unit.isNotEmpty()) Text(" [$unit]", fontSize = 11.sp, color = Pal.axis)
            Spacer(Modifier.width(8.dp))
            for (s in series) {
                Canvas(Modifier.size(10.dp)) { drawRect(s.color) }
                Text(" ${s.label}  ", fontSize = 11.sp, color = Pal.axis)
            }
        }
        Canvas(Modifier.fillMaxWidth().height(height.dp)) { drawLineChart(series, xWindow, yMin, yMax, markers, skipNonPositive) }
    }
}

private fun DrawScope.drawLineChart(
    series: List<Series>,
    xWindow: Float?,
    yMinFixed: Float?,
    yMaxFixed: Float?,
    markers: List<Marker>,
    skipNonPositive: Boolean,
) {
    val left = 40.dp.toPx()
    val bottom = 16.dp.toPx()
    val w = size.width - left - 6f
    val h = size.height - bottom - 4f
    if (w <= 0 || h <= 0) return

    var xMax = Float.NEGATIVE_INFINITY
    var xMin = Float.POSITIVE_INFINITY
    for (s in series) if (s.x.isNotEmpty()) {
        xMax = max(xMax, s.x.last())
        xMin = min(xMin, s.x.first())
    }
    if (xMax == Float.NEGATIVE_INFINITY) {
        drawRect(Pal.grid, Offset(left, 4f), Size(w, h), style = Stroke(1f))
        drawText("keine Daten", left + 8f, 4f + h / 2, Pal.axis)
        return
    }
    if (xWindow != null) xMin = max(xMin, xMax - xWindow)
    if (xMax - xMin < 1e-3f) xMax = xMin + 1f

    var yMin = yMinFixed ?: Float.POSITIVE_INFINITY
    var yMax = yMaxFixed ?: Float.NEGATIVE_INFINITY
    if (yMinFixed == null || yMaxFixed == null) {
        for (s in series) for (i in s.x.indices) {
            if (s.x[i] < xMin) continue
            val v = s.y[i]
            if (v.isNaN() || (skipNonPositive && v <= 0f)) continue
            if (yMinFixed == null) yMin = min(yMin, v)
            if (yMaxFixed == null) yMax = max(yMax, v)
        }
    }
    if (yMin == Float.POSITIVE_INFINITY) yMin = 0f
    if (yMax == Float.NEGATIVE_INFINITY) yMax = 1f
    if (yMax - yMin < 1e-6f) {
        yMax += 0.5f
        yMin -= 0.5f
    } else if (yMinFixed == null || yMaxFixed == null) {
        val pad = (yMax - yMin) * 0.06f
        if (yMinFixed == null) yMin -= pad
        if (yMaxFixed == null) yMax += pad
    }

    fun px(x: Float) = left + (x - xMin) / (xMax - xMin) * w
    fun py(y: Float) = 4f + h - (y - yMin) / (yMax - yMin) * h

    // Grid + labels
    val ys = niceStep(yMax - yMin, 4)
    var gy = ceil(yMin / ys) * ys
    while (gy <= yMax) {
        val y = py(gy)
        drawLine(Pal.grid, Offset(left, y), Offset(left + w, y), 1f)
        drawText(fmt(gy, ys), 2f, y + 4.dp.toPx(), Pal.axis, right = left - 6f)
        gy += ys
    }
    val xs = niceStep(xMax - xMin, 5)
    var gx = ceil(xMin / xs) * xs
    while (gx <= xMax) {
        val x = px(gx)
        drawLine(Pal.grid, Offset(x, 4f), Offset(x, 4f + h), 1f)
        drawText(fmt(gx, xs), x - 12f, size.height - 2f, Pal.axis)
        gx += xs
    }
    for (m in markers) {
        if (m.x < xMin || m.x > xMax) continue
        drawLine(m.color, Offset(px(m.x), 4f), Offset(px(m.x), 4f + h), 2f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 6f)))
    }

    // Curves (with many points every n-th; do not connect gaps > 2 s)
    for (s in series) {
        val n = s.x.size
        if (n == 0) continue
        var start = 0
        while (start < n && s.x[start] < xMin) start++
        val stride = max(1, (n - start) / (w.toInt() * 2).coerceAtLeast(1))
        if (s.dots) {
            var i = start
            while (i < n) {
                val v = s.y[i]
                if (!v.isNaN() && !(skipNonPositive && v <= 0f)) drawCircle(s.color, 3f, Offset(px(s.x[i]), py(v.coerceIn(yMin, yMax))))
                i += stride
            }
            continue
        }
        val path = Path()
        var pen = false
        var lastX = Float.NaN
        var i = start
        while (i < n) {
            val v = s.y[i]
            val x = s.x[i]
            if (v.isNaN() || (skipNonPositive && v <= 0f) || (!lastX.isNaN() && x - lastX > 2f)) pen = false
            if (!v.isNaN() && !(skipNonPositive && v <= 0f)) {
                val p = Offset(px(x), py(v.coerceIn(yMin, yMax)))
                if (pen) path.lineTo(p.x, p.y) else path.moveTo(p.x, p.y)
                pen = true
                lastX = x
            }
            i += stride
        }
        drawPath(path, s.color, style = Stroke(2.2f))
    }
    drawRect(Pal.grid, Offset(left, 4f), Size(w, h), style = Stroke(1f))
}

internal fun DrawScope.drawText(
    text: String,
    x: Float,
    y: Float,
    color: Color,
    sizePx: Float = 11f * density,
    right: Float? = null,
) {
    val paint =
        android.graphics.Paint().apply {
            this.color = android.graphics.Color.argb((color.alpha * 255).toInt(), (color.red * 255).toInt(), (color.green * 255).toInt(), (color.blue * 255).toInt())
            textSize = sizePx
            isAntiAlias = true
            if (right != null) textAlign = android.graphics.Paint.Align.RIGHT
        }
    drawContext.canvas.nativeCanvas.drawText(text, right ?: x, y, paint)
}

/** Bar/line profile over distance with target markers */
@Composable
fun ProfileChart(
    title: String,
    xMm: FloatArray,
    series: List<Pair<FloatArray, Color>>,
    labels: List<String>,
    markersMm: List<Pair<Float, Color>> = emptyList(),
    band: Pair<Float, Float>? = null,
    height: Int = 170,
    logY: Boolean = false,
) {
    val s =
        series.mapIndexed { i, (y, c) ->
            Series(xMm.map { it / 1000f }.toFloatArray(), if (logY) y.map { 20f * log10(max(it, 1f)) }.toFloatArray() else y, c, labels.getOrElse(i) { "" })
        }
    Column(Modifier.fillMaxWidth()) {
        LineChart(
            title,
            s,
            height = height,
            unit = if (logY) "dB über m" else "über m",
            markers =
                markersMm.map { Marker(it.first / 1000f, it.second) } +
                    (band?.let { listOf(Marker(it.first / 1000f, Pal.warn), Marker(it.second / 1000f, Pal.warn)) } ?: emptyList()),
        )
    }
}

/** Color scale for the waterfall (dB over noise, 0…[maxDb]) */
private fun heat(v: Float, maxDb: Float): Int {
    val t = (v / maxDb).coerceIn(0f, 1f)
    // dark blue → turquoise → yellow → red
    val r: Float
    val g: Float
    val b: Float
    when {
        t < 0.33f -> {
            val k = t / 0.33f
            r = 0.05f; g = 0.08f + 0.6f * k; b = 0.2f + 0.5f * k
        }
        t < 0.66f -> {
            val k = (t - 0.33f) / 0.33f
            r = 0.05f + 0.9f * k; g = 0.68f + 0.27f * k; b = 0.7f - 0.6f * k
        }
        else -> {
            val k = (t - 0.66f) / 0.34f
            r = 0.95f + 0.05f * k; g = 0.95f - 0.75f * k; b = 0.1f
        }
    }
    return android.graphics.Color.rgb((r * 255).toInt(), (g * 255).toInt(), (b * 255).toInt())
}

/** Waterfall: rows = time (newest at the bottom), columns = distance. */
@Composable
fun Waterfall(
    title: String,
    rows: List<FloatArray>,
    startMm: Float,
    stepMm: Float,
    maxDb: Float = 30f,
    height: Int = 260,
    markerMm: List<Float> = emptyList(),
) {
    val bmp: ImageBitmap? =
        remember(rows.size, rows.lastOrNull()) {
            if (rows.isEmpty()) {
                null
            } else {
                val cols = rows.maxOf { it.size }
                val b = android.graphics.Bitmap.createBitmap(cols, rows.size, android.graphics.Bitmap.Config.ARGB_8888)
                val px = IntArray(cols * rows.size)
                for ((r, row) in rows.withIndex()) for (c in 0 until cols) px[r * cols + c] = heat(row.getOrElse(c) { 0f }, maxDb)
                b.setPixels(px, 0, cols, 0, 0, cols, rows.size)
                b.asImageBitmap()
            }
        }
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Text(title, style = MaterialTheme.typography.labelLarge)
        Canvas(Modifier.fillMaxWidth().height(height.dp)) {
            val left = 40f
            val bottom = 30f
            val w = size.width - left
            val h = size.height - bottom
            if (bmp == null) {
                drawText("keine Raumprofile (iq3 starten)", left + 8f, h / 2, Pal.axis)
                return@Canvas
            }
            drawImage(
                bmp,
                srcOffset = androidx.compose.ui.unit.IntOffset.Zero,
                srcSize = IntSize(bmp.width, bmp.height),
                dstOffset = androidx.compose.ui.unit.IntOffset(left.toInt(), 0),
                dstSize = IntSize(w.toInt(), h.toInt()),
                filterQuality = androidx.compose.ui.graphics.FilterQuality.None,
            )
            val endMm = startMm + stepMm * (bmp.width - 1)
            var m = ceil(startMm / 500f) * 500f
            while (m <= endMm) {
                val x = left + (m - startMm) / (endMm - startMm) * w
                drawLine(Pal.grid, Offset(x, 0f), Offset(x, h), 1f)
                drawText("%.1f".format(m / 1000f), x - 12f, size.height - 4f, Pal.axis)
                m += 500f
            }
            for (mm in markerMm) {
                val x = left + (mm - startMm) / (endMm - startMm) * w
                drawLine(Color.White, Offset(x, h - 14f), Offset(x, h), 3f)
            }
            drawText("m", size.width - 20f, size.height - 4f, Pal.axis)
        }
    }
}

/** Key figure with title */
@Composable
fun Stat(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    color: Color = Color.Unspecified,
    sub: String? = null,
) {
    Column(modifier.padding(6.dp)) {
        Text(label, fontSize = 11.sp, color = Pal.axis)
        Text(value, fontSize = 22.sp, color = color)
        if (sub != null) Text(sub, fontSize = 11.sp, color = Pal.axis)
    }
}

internal fun f1(v: Float?): String = if (v == null || v.isNaN()) "–" else "%.1f".format(v)

internal fun f0(v: Float?): String = if (v == null || v.isNaN()) "–" else "%.0f".format(v)

internal fun near(a: Float, b: Float, tol: Float) = abs(a - b) <= tol
