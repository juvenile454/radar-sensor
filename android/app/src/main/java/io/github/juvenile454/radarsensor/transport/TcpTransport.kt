package io.github.juvenile454.radarsensor.transport

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.concurrent.thread

/** Development forwarding: ESP32 on the PC, `host/esp_tcp_bridge.py` provides it over TCP (Tailscale only). */
class TcpTransport(
    private val host: String,
    private val port: Int,
) : Transport {
    override val label = "TCP $host:$port"
    override val kind = "tcp"

    @Volatile private var socket: Socket? = null
    private var out: OutputStream? = null

    @Volatile private var closing = false

    override fun start(listener: Transport.Listener) {
        val s = Socket()
        s.tcpNoDelay = true
        s.connect(InetSocketAddress(host, port), 4000)
        socket = s
        out = s.getOutputStream()
        val input: InputStream = s.getInputStream()
        thread(name = "tcp-reader", isDaemon = true) {
            val buf = ByteArray(16 * 1024)
            var err: Exception? = null
            try {
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) {
                        err = IOException("Gegenstelle hat die Verbindung beendet")
                        break
                    }
                    if (n > 0) listener.onData(buf, n)
                }
            } catch (e: IOException) {
                err = e
            }
            listener.onClosed(if (closing) null else err)
        }
    }

    override fun write(data: ByteArray) {
        out?.apply {
            write(data)
            flush()
        }
    }

    override fun close() {
        closing = true
        try {
            socket?.close()
        } catch (_: IOException) {
        }
        socket = null
    }
}
