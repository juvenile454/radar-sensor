package de.timo.radarsensor.protocol

import org.junit.Assert.assertEquals
import org.junit.Test

class LineAssemblerTest {
    @Test fun zeilenUeberPaketgrenzen() {
        val out = mutableListOf<String>()
        val a = LineAssembler { out += it }
        val text = "@IQ3,1,2\r\n@ROOM,1\n\n@END\n@HALB"
        // in 3-byte chunks, like small USB packets
        val b = text.toByteArray()
        var i = 0
        while (i < b.size) {
            val n = minOf(3, b.size - i)
            a.feed(b.copyOfRange(i, i + n))
            i += n
        }
        assertEquals(listOf("@IQ3,1,2", "@ROOM,1", "@END"), out)
        a.feed(",fertig\n".toByteArray())
        assertEquals("@HALB,fertig", out.last())
    }

    @Test fun pufferMitLaenge() {
        val out = mutableListOf<String>()
        val a = LineAssembler { out += it }
        val buf = "abc\ndefXXXX".toByteArray()
        a.feed(buf, 7)
        a.feed("\n".toByteArray())
        assertEquals(listOf("abc", "def"), out)
    }

    @Test fun ueberlangeZeileWirdGeteilt() {
        val out = mutableListOf<String>()
        val a = LineAssembler(maxLine = 10) { out += it }
        a.feed("0123456789ABCDEF\nok\n".toByteArray())
        assertEquals(listOf("0123456789", "ABCDEF", "ok"), out)
        assertEquals(1, a.overlongLines)
    }

    @Test fun utf8() {
        val out = mutableListOf<String>()
        val a = LineAssembler { out += it }
        val b = "Größe µm\n".toByteArray()
        for (x in b) a.feed(byteArrayOf(x))
        assertEquals(listOf("Größe µm"), out)
    }
}
