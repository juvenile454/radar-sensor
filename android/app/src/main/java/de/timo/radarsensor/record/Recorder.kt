package de.timo.radarsensor.record

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
 * Schreibt eine Aufnahme im selben Format wie host/esp_record.py: `stream.log` (Zeilen wie vom ESP32),
 * `rxtime.csv` (line,rx_ms seit Start), `meta.json`. Puffer werden jede Sekunde geleert, damit ein Absturz
 * höchstens die letzte Sekunde kostet. Aufrufe aus dem Lese-Thread und der Oberfläche, daher synchronisiert.
 */
class Recorder private constructor(
    val dir: File,
    private val meta: RecordingMeta,
    private val startNanos: Long,
) {
    private val log: BufferedWriter = File(dir, "stream.log").bufferedWriter(bufferSize = 64 * 1024)
    private val rx: BufferedWriter = File(dir, "rxtime.csv").bufferedWriter(bufferSize = 16 * 1024)
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
            lastFlush = rxNanos
        }
    }

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

        /** Aufnahmen im Ordner, neueste zuerst (Unterordner mit stream.log). */
        fun list(root: File): List<File> =
            (root.listFiles() ?: emptyArray())
                .filter { File(it, "stream.log").isFile }
                .sortedByDescending { it.name }
    }
}
