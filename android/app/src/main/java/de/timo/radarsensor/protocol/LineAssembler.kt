package de.timo.radarsensor.protocol

import java.io.ByteArrayOutputStream

/**
 * Setzt Bytes aus beliebig großen Stücken (USB-Pakete, TCP) zu Zeilen zusammen. `\r` wird entfernt, leere Zeilen
 * entfallen. Überlange Zeilen (ohne `\n`, etwa bei Binärdaten im `bridge`-Modus) werden nach [maxLine] Bytes
 * abgeschnitten ausgegeben, damit der Puffer nicht wächst. Nicht threadsicher (ein Lese-Thread).
 */
class LineAssembler(
    private val maxLine: Int = 16 * 1024,
    private val onLine: (String) -> Unit,
) {
    private val buf = ByteArrayOutputStream(2048)

    var overlongLines = 0
        private set

    fun feed(
        data: ByteArray,
        len: Int = data.size,
    ) {
        var start = 0
        for (i in 0 until len) {
            if (data[i] == NL) {
                buf.write(data, start, i - start)
                emit()
                start = i + 1
            } else if (buf.size() + (i - start) >= maxLine) {
                // schon maxLine Bytes ohne Zeilenende: bis vor dieses Byte ausgeben, es beginnt die nächste Zeile
                buf.write(data, start, i - start)
                overlongLines++
                emit()
                start = i
            }
        }
        if (start < len) buf.write(data, start, len - start)
    }

    private fun emit() {
        var s = buf.toString(Charsets.UTF_8)
        buf.reset()
        if (s.endsWith('\r')) s = s.dropLast(1)
        if (s.isNotEmpty()) onLine(s)
    }

    private companion object {
        const val NL = '\n'.code.toByte()
    }
}
