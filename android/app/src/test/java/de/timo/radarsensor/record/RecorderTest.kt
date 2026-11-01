package de.timo.radarsensor.record

import de.timo.radarsensor.reference.HrMeasurement
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File
import java.nio.file.Files

class RecorderTest {
    private fun recorder(root: File) =
        Recorder.start(root, "t", "test", "dev", "replay", "replay", null, nowNanos = 1_000_000_000L)

    /** V0.2: Marker mit Empfangszeit (ms seit Aufnahmebeginn, wie rxtime.csv) in Reihenfolge. */
    @Test fun writesEventsCsv() {
        val root = Files.createTempDirectory("rec").toFile()
        val r = recorder(root)
        r.mark(1_500_000_000L, "ein")
        r.mark(2_250_000_000L, "anhalten_start")
        r.stop(3_000_000_000L)
        assertEquals(listOf("rx_ms,text", "500.0,ein", "1250.0,anhalten_start"), File(r.dir, "events.csv").readLines())
    }

    /** V0.1: je RR-Wert eine Zeile, ohne RR eine Zeile mit leerem rr_ms; Hautkontakt 1/0/leer. */
    @Test fun writesRefHrCsv() {
        val root = Files.createTempDirectory("rec").toFile()
        val r = recorder(root)
        r.reference(2_000_000_000L, HrMeasurement(72, listOf(1000.0, 750.0), true))
        r.reference(3_000_000_000L, HrMeasurement(70, emptyList(), null))
        r.stop(4_000_000_000L)
        assertEquals(
            listOf("rx_ms,hr_bpm,rr_ms,contact", "1000.0,72,1000.0,1", "1000.0,72,750.0,1", "2000.0,70,,"),
            File(r.dir, "ref_hr.csv").readLines(),
        )
    }

    /** Ohne Marker oder Gurt entstehen die Dateien nicht (alte Aufnahmen sehen gleich aus). */
    @Test fun noOptionalFilesWithoutData() {
        val root = Files.createTempDirectory("rec").toFile()
        val r = recorder(root)
        r.stop(2_000_000_000L)
        assertEquals(false, File(r.dir, "events.csv").exists())
        assertEquals(false, File(r.dir, "ref_hr.csv").exists())
    }
}
