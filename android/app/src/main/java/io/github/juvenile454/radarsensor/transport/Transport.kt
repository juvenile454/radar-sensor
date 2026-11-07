package io.github.juvenile454.radarsensor.transport

/**
 * Connection to the ESP32: USB-CDC now, TCP for development, replay of recordings; BLE later as another
 * implementation. [start] opens and reads in the background; [Listener] is called on the read thread.
 */
interface Transport {
    /** Short description for display and meta.json, e.g. "USB 303a:1001 EC:DA:…" */
    val label: String

    /** "usb", "tcp", "replay" */
    val kind: String

    interface Listener {
        fun onData(
            data: ByteArray,
            len: Int,
        )

        /** Connection lost or end of replay (`null`). */
        fun onClosed(error: Exception?)
    }

    /** Opens the connection (blocking) and starts reading. Throws on errors. */
    fun start(listener: Listener)

    /** Sends bytes to the ESP32 (no effect during replay). */
    fun write(data: ByteArray)

    fun close()
}
