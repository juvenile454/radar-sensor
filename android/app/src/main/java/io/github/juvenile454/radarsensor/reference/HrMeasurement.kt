package io.github.juvenile454.radarsensor.reference

/**
 * A notification of the Heart Rate Measurement characteristic (0x2A37, Bluetooth Heart Rate Service), e.g. from the
 * ECG chest strap Polar H9/H10 (V0.1). rrMs: RR intervals since the last notification in ms (strap resolution 1/1024 s).
 * contact: skin contact, null = not supported by the strap.
 */
data class HrMeasurement(
    val bpm: Int,
    val rrMs: List<Double>,
    val contact: Boolean?,
)

/**
 * Flags (byte 0): bit 0 = pulse as uint16 (else uint8), bit 2 = skin contact supported, bit 1 = contact detected,
 * bit 3 = Energy Expended follows (2 bytes, skipped), bit 4 = RR values follow (each uint16 little-endian in 1/1024 s).
 * null if the message is shorter than the pulse.
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
