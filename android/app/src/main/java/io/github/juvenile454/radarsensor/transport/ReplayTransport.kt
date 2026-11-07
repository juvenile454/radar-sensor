package io.github.juvenile454.radarsensor.transport

import java.io.BufferedReader
import java.io.File
import kotlin.concurrent.thread

/**
 * Plays back a recording (`stream.log`, optionally `rxtime.csv`) at the original pace, sped up by [speed].
 * Without rxtime.csv (old PC logs) 1/60 s per `@IQ3` line applies, all other lines immediately. Reads line by line,
 * so even long recordings with raw data fit in memory.
 */
class ReplayTransport(
    private val dir: File,
    private val speed: Float = 1f,
    private val loop: Boolean = false,
) : Transport {
    override val label = "Wiedergabe ${dir.name}" + if (speed != 1f) " ×${speed.toInt()}" else ""
    override val kind = "replay"

    @Volatile private var running = false

    override fun start(listener: Transport.Listener) {
        val log = if (dir.isFile) dir else File(dir, "stream.log")
        require(log.isFile) { "keine stream.log in ${dir.name}" }
        val rxFile = File(log.parentFile, "rxtime.csv").takeIf { it.isFile }
        running = true
        thread(name = "replay", isDaemon = true) {
            var err: Exception? = null
            try {
                do {
                    playOnce(log, rxFile, listener)
                } while (running && loop)
            } catch (e: Exception) {
                err = e
            }
            running = false
            listener.onClosed(err)
        }
    }

    private fun playOnce(
        log: File,
        rxFile: File?,
        listener: Transport.Listener,
    ) {
        val rx: BufferedReader? = rxFile?.bufferedReader()?.also { it.readLine() } // header line
        log.bufferedReader().use { r ->
            val t0 = System.nanoTime()
            var paced = 0.0
            var lastDue = 0.0
            while (running) {
                val line = r.readLine() ?: break
                val rxLine = rx?.readLine()
                val dueMs =
                    if (rxLine != null) {
                        rxLine.substringAfter(',').toDoubleOrNull() ?: lastDue
                    } else {
                        if (line.startsWith("@IQ3,")) paced += 1000.0 / 60.0
                        paced
                    }
                lastDue = dueMs
                val waitMs = dueMs / speed - (System.nanoTime() - t0) / 1e6
                if (waitMs > 1) Thread.sleep(waitMs.toLong())
                val b = (line + "\n").toByteArray()
                listener.onData(b, b.size)
            }
        }
        rx?.close()
    }

    override fun write(data: ByteArray) = Unit

    override fun close() {
        running = false
    }
}
