package de.timo.radarsensor.reference

/**
 * Eine Benachrichtigung der Characteristic Heart Rate Measurement (0x2A37, Bluetooth Heart Rate Service), z. B. vom
 * EKG-Gurt Polar H10 (V0.1). rrMs: RR-Intervalle seit der letzten Benachrichtigung in ms (Gurt-Auflösung 1/1024 s).
 * contact: Hautkontakt, null = vom Gurt nicht unterstützt.
 */
data class HrMeasurement(
    val bpm: Int,
    val rrMs: List<Double>,
    val contact: Boolean?,
)

/**
 * Flags (Byte 0): Bit 0 = Puls als uint16 (sonst uint8), Bit 2 = Hautkontakt unterstützt, Bit 1 = Kontakt erkannt,
 * Bit 3 = Energy Expended folgt (2 Byte, übersprungen), Bit 4 = RR-Werte folgen (je uint16 little-endian in 1/1024 s).
 * null, wenn die Nachricht kürzer als der Puls ist.
 */
fun parseHrMeasurement(bytes: ByteArray): HrMeasurement? {
    if (bytes.size < 2) return null
    val flags = bytes[0].toInt() and 0xFF
    val wide = (flags and 0x01) != 0
    if (wide && bytes.size < 3) return null
    val bpm = if (wide) u16(bytes, 1) else bytes[1].toInt() and 0xFF
    var i = if (wide) 3 else 2
    val contact = if ((flags and 0x04) != 0) (flags and 0x02) != 0 else null
    if ((flags and 0x08) != 0) i += 2
    val rr = ArrayList<Double>(4)
    if ((flags and 0x10) != 0) {
        while (i + 1 < bytes.size) {
            rr += u16(bytes, i) * 1000.0 / 1024.0
            i += 2
        }
    }
    return HrMeasurement(bpm, rr, contact)
}

private fun u16(
    b: ByteArray,
    i: Int,
): Int = (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)
