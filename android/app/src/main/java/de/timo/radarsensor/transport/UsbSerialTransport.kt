package de.timo.radarsensor.transport

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import com.hoho.android.usbserial.driver.CdcAcmSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import java.io.IOException

/**
 * USB-CDC to the ESP32-S3 (native USB port, USB-Serial/JTAG `303a:1001`).
 *
 * Reset trap: the USB-Serial/JTAG block restarts the chip when RTS is set and DTR is cleared (checked on the PC with
 * `@HELLO up_ms`, 2026-11-01). Therefore only set DTR and never touch RTS.
 */
class UsbSerialTransport(
    private val usb: UsbManager,
    private val driver: UsbSerialDriver,
) : Transport {
    private val dev: UsbDevice = driver.device
    override val label = "USB %04x:%04x %s".format(dev.vendorId, dev.productId, safeSerial(dev))
    override val kind = "usb"

    private var port: UsbSerialPort? = null
    private var io: SerialInputOutputManager? = null

    @Volatile private var closing = false

    override fun start(listener: Transport.Listener) {
        val conn = usb.openDevice(dev) ?: throw IOException("USB-Gerät lässt sich nicht öffnen (Berechtigung?)")
        val p = driver.ports.first()
        p.open(conn)
        try {
            p.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            p.dtr = true
        } catch (e: Exception) {
            p.close()
            throw e
        }
        port = p
        val m =
            SerialInputOutputManager(
                p,
                object : SerialInputOutputManager.Listener {
                    override fun onNewData(data: ByteArray) = listener.onData(data, data.size)

                    override fun onRunError(e: Exception) = listener.onClosed(if (closing) null else e)
                },
            )
        m.readBufferSize = 16 * 1024
        io = m
        m.start()
    }

    override fun write(data: ByteArray) {
        port?.write(data, 500) ?: throw IOException("nicht verbunden")
    }

    override fun close() {
        closing = true
        io?.stop()
        io = null
        try {
            port?.close()
        } catch (_: IOException) {
        }
        port = null
    }

    companion object {
        const val ESPRESSIF_VID = 0x303a
        const val ESP32S3_USB_JTAG_PID = 0x1001

        private val prober: UsbSerialProber by lazy {
            val table = UsbSerialProber.getDefaultProbeTable()
            table.addProduct(ESPRESSIF_VID, ESP32S3_USB_JTAG_PID, CdcAcmSerialDriver::class.java)
            UsbSerialProber(table)
        }

        /** All serial USB devices, ESP32 first. */
        fun findDrivers(usb: UsbManager): List<UsbSerialDriver> =
            prober.findAllDrivers(usb).sortedByDescending { it.device.vendorId == ESPRESSIF_VID }

        fun describe(d: UsbDevice): String =
            "%04x:%04x %s %s".format(d.vendorId, d.productId, d.productName ?: "", safeSerial(d)).trim()

        /** Reading the serial number needs the USB permission, otherwise SecurityException. */
        fun safeSerial(d: UsbDevice): String =
            try {
                d.serialNumber ?: ""
            } catch (_: SecurityException) {
                ""
            }
    }
}
