package de.timo.radarsensor.protocol

import java.io.ByteArrayOutputStream

/**
 * Assembles bytes from chunks of any size (USB packets, TCP) into lines. `\r` is removed, empty lines
 * are dropped. Overlong lines (without `\n`, e.g. with binary data in `bridge` mode) are emitted truncated after
 * [maxLine] bytes so that the buffer does not grow. Not thread-safe (one read thread).
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
                // already maxLine bytes without a line ending: emit up to before this byte, it starts the next line
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
