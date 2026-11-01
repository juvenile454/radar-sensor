package de.timo.radarsensor.transport

/**
 * Verbindung zum ESP32: USB-CDC jetzt, TCP für die Entwicklung, Wiedergabe von Aufnahmen; BLE später als weitere
 * Umsetzung. [start] öffnet und liest im Hintergrund; [Listener] wird im Lese-Thread aufgerufen.
 */
interface Transport {
    /** Kurzbeschreibung für Anzeige und meta.json, z. B. "USB 303a:1001 EC:DA:…" */
    val label: String

    /** "usb", "tcp", "replay" */
    val kind: String

    interface Listener {
        fun onData(
            data: ByteArray,
            len: Int,
        )

        /** Verbindung verloren oder Ende der Wiedergabe (`null`). */
        fun onClosed(error: Exception?)
    }

    /** Öffnet die Verbindung (blockierend) und startet das Lesen. Wirft bei Fehlern. */
    fun start(listener: Listener)

    /** Sendet Bytes an den ESP32 (bei Wiedergabe ohne Wirkung). */
    fun write(data: ByteArray)

    fun close()
}
