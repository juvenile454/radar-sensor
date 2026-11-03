package de.timo.radarsensor.reference

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.UUID

/** Found chest strap (BLE advertisement with Heart Rate Service). */
data class HrDevice(
    val address: String,
    val name: String?,
    val rssi: Int,
)

sealed interface RefState {
    data object Idle : RefState

    data object Scanning : RefState

    data class Connecting(
        val address: String,
    ) : RefState

    data class Connected(
        val address: String,
        val name: String?,
    ) : RefState

    data class Failed(
        val message: String,
    ) : RefState
}

/**
 * ECG chest strap over BLE (V0.1), e.g. Polar H10: scan for devices with Heart Rate Service (0x180D), connect,
 * enable notifications of the Heart Rate Measurement (0x2A37), pass every notification with its receive time
 * (elapsedRealtimeNanos, same time axis as rxtime.csv) to [onMeasurement]. If the connection drops, Android
 * reconnects by itself (autoConnect) until [disconnect] is called; likewise after a failed
 * service discovery (at most [MAX_DISCOVERY_FAILURES] in a row, [discoveryAction]). The permissions BLUETOOTH_SCAN
 * (neverForLocation) and BLUETOOTH_CONNECT are requested by the activity; if they are missing, [state] reports Failed.
 */
@SuppressLint("MissingPermission")
class HrReference(
    private val context: Context,
    private val onMeasurement: (rxNanos: Long, m: HrMeasurement) -> Unit,
    private val log: (String) -> Unit,
) {
    private val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
    private val main = Handler(Looper.getMainLooper())

    private val _state = MutableStateFlow<RefState>(RefState.Idle)
    val state: StateFlow<RefState> = _state.asStateFlow()

    private val _found = MutableStateFlow<List<HrDevice>>(emptyList())
    val found: StateFlow<List<HrDevice>> = _found.asStateFlow()

    /** Last notification (for status and display). */
    @Volatile var last: HrMeasurement? = null
        private set

    @Volatile var notifications = 0L
        private set

    @Volatile private var gatt: BluetoothGatt? = null

    @Volatile private var userClosed = false

    @Volatile private var discoveryFailures = 0
    private var scanning = false

    private val scanCb =
        object : ScanCallback() {
            override fun onScanResult(
                callbackType: Int,
                result: ScanResult,
            ) {
                val d = HrDevice(result.device.address, result.scanRecord?.deviceName ?: result.device.name, result.rssi)
                _found.value = (_found.value.filter { it.address != d.address } + d).sortedByDescending { it.rssi }
            }

            override fun onScanFailed(errorCode: Int) {
                scanning = false
                fail("BLE-Suche fehlgeschlagen (Fehler $errorCode)")
            }
        }

    /** Scan for chest straps (for durationMs); results in [found], strongest signal first. */
    fun scan(durationMs: Long = 10_000L) {
        val sc = adapter?.bluetoothLeScanner
        if (adapter == null || !adapter.isEnabled || sc == null) {
            fail("Bluetooth aus oder nicht vorhanden")
            return
        }
        stopScan()
        _found.value = emptyList()
        try {
            val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(HR_SERVICE)).build()
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
            sc.startScan(listOf(filter), settings, scanCb)
        } catch (e: SecurityException) {
            fail("Bluetooth-Berechtigung fehlt (${e.message})")
            return
        }
        scanning = true
        if (gatt == null) _state.value = RefState.Scanning
        log("Gurt-Suche ${durationMs / 1000} s")
        main.postDelayed({ stopScan() }, durationMs)
    }

    fun stopScan() {
        if (!scanning) return
        scanning = false
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCb)
        } catch (_: SecurityException) {
        }
        if (_state.value == RefState.Scanning) _state.value = RefState.Idle
        log("Gurt-Suche beendet: ${_found.value.size} gefunden")
    }

    /** Connect to the chest strap at address (MAC); an existing connection is disconnected first. */
    fun connect(address: String) {
        stopScan()
        closeGatt()
        val a = adapter
        if (a == null || !a.isEnabled) {
            fail("Bluetooth aus oder nicht vorhanden")
            return
        }
        val dev =
            try {
                a.getRemoteDevice(address.uppercase())
            } catch (e: IllegalArgumentException) {
                fail("ungültige Adresse: $address")
                return
            }
        userClosed = false
        discoveryFailures = 0
        _state.value = RefState.Connecting(dev.address)
        log("verbinde Gurt ${dev.address}")
        try {
            gatt = dev.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            fail("Bluetooth-Berechtigung fehlt (${e.message})")
        }
    }

    /** By the user: disconnect, do not reconnect. */
    fun disconnect() {
        userClosed = true
        stopScan()
        closeGatt()
        _state.value = RefState.Idle
        log("Gurt getrennt")
    }

    private fun closeGatt() {
        val g = gatt ?: return
        gatt = null
        try {
            g.disconnect()
            g.close()
        } catch (_: SecurityException) {
        }
    }

    private fun fail(msg: String) {
        _state.value = RefState.Failed(msg)
        log(msg)
    }

    private val gattCb =
        object : BluetoothGattCallback() {
            override fun onConnectionStateChange(
                g: BluetoothGatt,
                status: Int,
                newState: Int,
            ) {
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> g.discoverServices()
                    BluetoothProfile.STATE_DISCONNECTED -> {
                        if (gatt !== g) return
                        if (userClosed) return
                        // dropped (out of range, strap off): Android reconnects by itself as soon as it is there
                        g.close()
                        _state.value = RefState.Connecting(g.device.address)
                        log("Gurt-Verbindung verloren (Status $status), verbinde wieder")
                        gatt = g.device.connectGatt(context, true, this, BluetoothDevice.TRANSPORT_LE)
                    }
                }
            }

            override fun onServicesDiscovered(
                g: BluetoothGatt,
                status: Int,
            ) {
                val ch = if (status == GATT_OK) g.getService(HR_SERVICE)?.getCharacteristic(HR_MEASUREMENT) else null
                val cccd = ch?.getDescriptor(CCCD)
                when (discoveryAction(status, ch != null && cccd != null, discoveryFailures)) {
                    Discovery.Subscribe -> discoveryFailures = 0
                    Discovery.NoHrService -> {
                        fail("Gerät hat keinen Heart Rate Service")
                        userClosed = true
                        g.disconnect()
                        return
                    }
                    Discovery.Reconnect -> {
                        // disconnect; onConnectionStateChange then reconnects as after a drop
                        discoveryFailures++
                        log("Gurt-Dienstsuche fehlgeschlagen (Status $status), verbinde wieder")
                        g.disconnect()
                        return
                    }
                    Discovery.GiveUp -> {
                        fail("Gurt-Dienstsuche $MAX_DISCOVERY_FAILURES-mal fehlgeschlagen (Status $status)")
                        userClosed = true
                        g.disconnect()
                        return
                    }
                }
                if (ch == null || cccd == null) return
                g.setCharacteristicNotification(ch, true)
                g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                _state.value = RefState.Connected(g.device.address, g.device.name)
                log("Gurt verbunden: ${g.device.name ?: g.device.address}")
            }

            override fun onCharacteristicChanged(
                g: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                value: ByteArray,
            ) {
                val now = SystemClock.elapsedRealtimeNanos()
                if (characteristic.uuid != HR_MEASUREMENT) return
                val m = parseHrMeasurement(value) ?: return
                last = m
                notifications++
                onMeasurement(now, m)
            }
        }

    companion object {
        val HR_SERVICE: UUID = UUID.fromString("0000180d-0000-1000-8000-00805f9b34fb")
        val HR_MEASUREMENT: UUID = UUID.fromString("00002a37-0000-1000-8000-00805f9b34fb")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
