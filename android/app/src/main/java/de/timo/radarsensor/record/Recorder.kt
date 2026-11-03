package de.timo.radarsensor.record

import de.timo.radarsensor.reference.HrMeasurement
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.BufferedWriter
import java.io.File
import java.time.OffsetDateTime
import java.time.format.DateTimeFormatter

@Serializable
data class SentCommand(
    val line: Long,
    val rxMs: Double,
    val command: String,
)

@Serializable
data class RecordingMeta(
    val source: String = "android",
    val app: String,
    val device: String,
    val transport: String,
    val transportKind: String,
    val start: String,
    var hello: String? = null,
    var lines: Long = 0,
    var bytes: Long = 0,
    var durationS: Double = 0.0,
    var end: String? = null,
    val commands: MutableList<SentCommand> = mutableListOf(),
    var note: String? = null,
)

/**
 * Writes a recording in the same format as host/esp_record.py: `stream.log` (lines as from the ESP32),
 * `rxtime.csv` (line,rx_ms since start), `meta.json`; plus, as soon as there is data, `ref_hr.csv` (ECG chest strap, V0.1:
 * rx_ms,hr_bpm,rr_ms,contact, one line per RR value) and `events.csv` (markers, V0.2: rx_ms,text). All rx_ms on the
 * same time axis (ms since recording start, elapsedRealtimeNanos), which is how host/vital_eval.py evaluates them. Buffers are flushed every second, so that a crash
 * costs at most the last second. Called from the read thread and the UI, hence synchronized.
 */
class Recorder private constructor(
    val dir: File,
    private val meta: RecordingMeta,
    private val startNanos: Long,
) {
    private val log: BufferedWriter = File(dir, "stream.log").bufferedWriter(bufferSize = 64 * 1024)
    private val rx: BufferedWriter = File(dir, "rxtime.csv").bufferedWriter(bufferSize = 16 * 1024)
    private var refHr: BufferedWriter? = null
    private var events: BufferedWriter? = null
    private var lastFlush = startNanos
    private var closed = false

    init {
        rx.write("line,rx_ms\n")
        writeMeta()
    }

    val lines: Long get() = synchronized(this) { meta.lines }
    val bytes: Long get() = synchronized(this) { meta.bytes }

    fun line(
        text: String,
        rxNanos: Long,
    ) = synchronized(this) {
        if (closed) return
        if (meta.hello == null && text.startsWith("@HELLO")) meta.hello = text
        log.write(text)
        log.write("\n")
        rx.write("${meta.lines},${"%.1f".format(java.util.Locale.ROOT, (rxNanos - startNanos) / 1e6)}\n")
        meta.lines++
        meta.bytes += text.length + 1
        if (rxNanos - lastFlush > 1_000_000_000L) {
            log.flush()
            rx.flush()
            refHr?.flush()
            events?.flush()
            lastFlush = rxNanos
        }
    }

    /** One chest strap notification (V0.1): one line per RR value, without RR one line with empty rr_ms. */
    fun reference(
        rxNanos: Long,
        m: HrMeasurement,
    ) = synchronized(this) {
        if (closed) return
        val w = refHr ?: File(dir, "ref_hr.csv").bufferedWriter().also {
            it.write("rx_ms,hr_bpm,rr_ms,contact\n")
            refHr = it
        }
        val t = ms(rxNanos)
        val c = when (m.contact) {
            true -> "1"
            false -> "0"
            null -> ""
        }
        if (m.rrMs.isEmpty()) {
            w.write("$t,${m.bpm},,$c\n")
        } else {
            for (rr in m.rrMs) w.write("$t,${m.bpm},${"%.1f".format(java.util.Locale.ROOT, rr)},$c\n")
        }
    }

    /** Markers (V0.2), as host/vital_eval.py reads them: "ein" (metronome), "anhalten_start" / "anhalten_ende",
     *  "bewegung_start" / "bewegung_ende", "zaehlen_start" / "zaehlen_ende". */
    fun mark(
        rxNanos: Long,
        text: String,
    ) = synchronized(this) {
        if (closed) return
        val w = events ?: File(dir, "events.csv").bufferedWriter().also {
            it.write("rx_ms,text\n")
            events = it
        }
        w.write("${ms(rxNanos)},${text.replace(Regex("[,\\r\\n]+"), " ").trim()}\n")
    }

    private fun ms(nanos: Long) = "%.1f".format(java.util.Locale.ROOT, (nanos - startNanos) / 1e6)

    fun command(
        cmd: String,
        nowNanos: Long,
    ) = synchronized(this) {
        if (closed) return
        meta.commands += SentCommand(meta.lines, (nowNanos - startNanos) / 1e6, cmd)
    }

    fun stop(nowNanos: Long) =
        synchronized(this) {
            if (closed) return
            closed = true
            log.close()
            rx.close()
            refHr?.close()
            events?.close()
            meta.durationS = (nowNanos - startNanos) / 1e9
            meta.end = OffsetDateTime.now().format(DateTimeFormatter.ISO_OFFSET_DATE_TIME)
            writeMeta()
        }

    private fun writeMeta() {
        File(dir, "meta.json").writeText(JSON.encodeToString(RecordingMeta.serializer(), meta) + "\n")
    }

    companion object {
        private val JSON =
            Json {
                prettyPrint = true
                encodeDefaults = true
            }
        private val NAME_FMT = DateTimeFormatter.ofPattern("yyyy-MM-dd_HHmmss")

        fun start(
            root: File,
            label: String,
            app: String,
            device: String,
            transport: String,
            transportKind: String,
            hello: String?,
            nowNanos: Long,
        ): Recorder {
            val now = OffsetDateTime.now()
            val safe = label.replace(Regex("[^A-Za-z0-9_-]+"), "_").trim('_').take(40)
            val dir = File(root, now.format(NAME_FMT) + if (safe.isNotEmpty()) "_$safe" else "")
            check(dir.mkdirs()) { "Ordner nicht anlegbar: $dir" }
            val meta =
                RecordingMeta(
                    app = app,
                    device = device,
                    transport = transport,
                    transportKind = transportKind,
                    start = now.format(DateTimeFormatter.ISO_OFFSET_DATE_TIME),
                    hello = hello,
                )
            return Recorder(dir, meta, nowNanos)
        }

        /** Recordings in the folder, newest first (subfolders with stream.log). */
        fun list(root: File): List<File> =
            (root.listFiles() ?: emptyArray())
                .filter { File(it, "stream.log").isFile }
                .sortedByDescending { it.name }
    }
}
