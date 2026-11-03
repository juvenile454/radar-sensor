package de.timo.radarsensor

import de.timo.radarsensor.live.LiveStore
import de.timo.radarsensor.protocol.Iq3
import de.timo.radarsensor.protocol.LineAssembler
import de.timo.radarsensor.protocol.LineParser
import de.timo.radarsensor.protocol.Malformed
import de.timo.radarsensor.protocol.Raw
import de.timo.radarsensor.protocol.UnknownTag
import de.timo.radarsensor.record.Recorder
import de.timo.radarsensor.transport.ReplayTransport
import de.timo.radarsensor.transport.Transport
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs

/**
 * Checks parser, LiveStore, recording and replay against real recordings from the ESP32 (host/esp_record.py).
 * Stored outside the repo: RADAR_FIXTURES or ~/Data/radar-sensor/app-fixtures. If it is missing, the
 * tests are skipped (not counted as passed).
 */
class FixtureTest {
    private val root = File(System.getenv("RADAR_FIXTURES") ?: (System.getProperty("user.home") + "/Data/radar-sensor/app-fixtures"))

    private fun fixture(name: String): File {
        val d = File(root, name)
        assumeTrue("Testaufnahme fehlt: $d", File(d, "stream.log").isFile)
        return d
    }

    private fun tagCounts(f: File): Map<String, Int> =
        f.readLines().filter { it.startsWith("@") }.groupingBy { it.substringBefore(',') }.eachCount()

    @Test fun alleZeilenAllerAufnahmenLesbar() {
        assumeTrue(root.isDirectory)
        val dirs = root.listFiles()!!.filter { File(it, "stream.log").isFile }
        assumeTrue(dirs.isNotEmpty())
        var total = 0
        for (d in dirs) {
            for (line in File(d, "stream.log").readLines()) {
                val m = LineParser.parse(line)
                assertTrue("${d.name}: $m", m !is Malformed && m !is UnknownTag)
                total++
            }
        }
        println("FixtureTest: $total Zeilen aus ${dirs.size} Aufnahmen gelesen")
    }

    @Test fun rohdatenPassenZuIq3() {
        val d = fixture("2026-11-01_iq3raw_20s")
        val iq = HashMap<Long, Iq3>()
        val raws = ArrayList<Raw>()
        for (line in File(d, "stream.log").readLines()) {
            when (val m = LineParser.parse(line)) {
                is Iq3 -> iq[m.frame!!] = m
                is Raw -> raws += m
                else -> Unit
            }
        }
        assertTrue(raws.size > 1000)
        var maxDiff = 0f
        for (r in raws) {
            assertEquals(105, r.points)
            val q = iq[r.frame] ?: continue
            assertEquals(q.tick, r.tick)
            // like esp_record.py --check: loopback = point 0, zoom amplitude = middle of the 24 zoom points (1 + 12)
            maxDiff = maxOf(maxDiff, abs(r.amplitude(0) - q.loopbackAmp), abs(r.amplitude(13) - q.zoomAmp))
        }
        println("FixtureTest: ${raws.size} Rohframes, |IQ| vs. @IQ3 max. Abweichung $maxDiff")
        assertTrue("Abweichung $maxDiff", maxDiff <= 0.051f) // @IQ3 with one decimal place
    }

    @Test fun liveStoreZaehltWieDieDatei() {
        val d = fixture("2026-11-01_iq3_30s_reconnecttest")
        val log = File(d, "stream.log")
        val counts = tagCounts(log)
        val s = LiveStore(iqSeconds = 120)
        for (line in log.readLines()) s.apply(line, LineParser.parse(line))
        val snap = s.snapshot()
        assertEquals(counts["@IQ3"], snap.iqT.size)
        assertEquals(counts["@VITAL"], snap.vitalT.size)
        assertEquals(counts["@RECONNECT"], snap.reconnects)
        // Time axis does not run backwards despite the server restart (tick starts over)
        val t = snap.iqT
        for (i in 1 until t.size) assertTrue("Zeit rückwärts bei $i: ${t[i - 1]} → ${t[i]}", t[i] >= t[i - 1])
        assertTrue("Dauer ${t.last()} s", t.last() in 25f..35f)
        println("FixtureTest: Reconnect-Aufnahme ${t.size} Frames, ${snap.framesLost} verloren laut Zähler, ${t.last()} s")
    }

    @Test fun aufnahmeImPcFormatUndWiedergabe() {
        val src = fixture("2026-11-01_doppler_8s")
        val tmp = Files.createTempDirectory("radar-rec").toFile()
        try {
            // Replay (20x) → lines → Recorder, as in the app
            val rec = Recorder.start(tmp, "test doppler", "test", "JVM", "replay", "replay", null, System.nanoTime())
            val store = LiveStore()
            val done = CountDownLatch(1)
            val asm =
                LineAssembler {
                    rec.line(it, System.nanoTime())
                    store.apply(it, LineParser.parse(it))
                }
            val replay = ReplayTransport(src, speed = 20f)
            replay.start(
                object : Transport.Listener {
                    override fun onData(
                        data: ByteArray,
                        len: Int,
                    ) = asm.feed(data, len)

                    override fun onClosed(error: Exception?) {
                        assertEquals(null, error)
                        done.countDown()
                    }
                },
            )
            assertTrue("Wiedergabe hängt", done.await(20, TimeUnit.SECONDS))
            rec.command("doppler 8", System.nanoTime())
            rec.stop(System.nanoTime())

            val out = rec.dir
            assertTrue(out.name.endsWith("_test_doppler"))
            assertEquals(File(src, "stream.log").readLines(), File(out, "stream.log").readLines())
            val rx = File(out, "rxtime.csv").readLines()
            assertEquals("line,rx_ms", rx[0])
            assertEquals(File(src, "stream.log").readLines().size, rx.size - 1)
            val meta = File(out, "meta.json").readText()
            assertTrue(meta.contains("\"source\": \"android\""))
            assertTrue(meta.contains("\"command\": \"doppler 8\""))
            // Original pace / 20: source lasts ≈ 8 s
            val lastRx = rx.last().substringAfter(',').toDouble()
            assertTrue("Takt $lastRx ms", lastRx in 250.0..1500.0)
            assertEquals(tagCounts(File(src, "stream.log"))["@DOPPLER"], store.snapshot().dopT.size)
            assertEquals(listOf(out), Recorder.list(tmp))
        } finally {
            tmp.deleteRecursively()
        }
    }
}
