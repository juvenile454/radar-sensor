package de.timo.radarsensor.reference

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** V0.1: Heart Rate Measurement (0x2A37) eines EKG-Gurts, z. B. Polar H10. */
class HrMeasurementTest {
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    @Test fun parses8BitHrWithRr() {
        val m = parseHrMeasurement(bytes(0x10, 0x48, 0x00, 0x04))!!
        assertEquals(72, m.bpm)
        assertEquals(listOf(1000.0), m.rrMs)
    }

    @Test fun parses16BitHrTwoRr() {
        val m = parseHrMeasurement(bytes(0x11, 0x48, 0x00, 0x00, 0x04, 0x00, 0x03))!!
        assertEquals(72, m.bpm)
        assertEquals(listOf(1000.0, 750.0), m.rrMs)
    }

    @Test fun skipsEnergyExpended() {
        val m = parseHrMeasurement(bytes(0x18, 0x48, 0x10, 0x00, 0x00, 0x04))!!
        assertEquals(72, m.bpm)
        assertEquals(listOf(1000.0), m.rrMs)
    }

    @Test fun contactBits() {
        assertEquals(true, parseHrMeasurement(bytes(0x06, 0x48))!!.contact)
        assertEquals(false, parseHrMeasurement(bytes(0x04, 0x48))!!.contact)
        assertNull(parseHrMeasurement(bytes(0x00, 0x48))!!.contact)
    }

    @Test fun rejectsTooShort() {
        assertNull(parseHrMeasurement(bytes(0x10)))
    }
}
