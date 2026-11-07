package io.github.juvenile454.radarsensor.core

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.SystemClock
import android.util.Log
import io.github.juvenile454.radarsensor.BuildConfig
import io.github.juvenile454.radarsensor.dsp.DspReplay
import io.github.juvenile454.radarsensor.dsp.LiveDsp
import io.github.juvenile454.radarsensor.dsp.PhoneDsp
import io.github.juvenile454.radarsensor.live.LiveStore
import io.github.juvenile454.radarsensor.protocol.End
import io.github.juvenile454.radarsensor.protocol.LineAssembler
import io.github.juvenile454.radarsensor.protocol.LineParser
import io.github.juvenile454.radarsensor.protocol.SessEnd
import io.github.juvenile454.radarsensor.record.Recorder
import io.github.juvenile454.radarsensor.reference.HrMeasurement
import io.github.juvenile454.radarsensor.reference.HrReference
import io.github.juvenile454.radarsensor.transport.Transport
import io.github.juvenile454.radarsensor.transport.UsbSerialTransport
import io.github.juvenile454.radarsensor.ui.Metronome
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

sealed interface ConnState {
    data object Idle : ConnState

    data class Connecting(
        val label: String,
    ) : ConnState

    data class Connected(
        val label: String,
        val kind: String,
    ) : ConnState

    data class Failed(
        val message: String,
    ) : ConnState

    /** The remote end finished without error (end of replay, bridge terminated) */
    data class Ended(
        val label: String,
    ) : ConnState
}

data class LinkStats(
    val bytesPerS: Float = 0f,
    val linesPerS: Float = 0f,
    val totalBytes: Long = 0,
    val connectedSinceMs: Long = 0,
)

/**
 * Connection, data flow and recording, independent of the UI (lives in the process; the [RadarService] keeps it
 * in the foreground while a connection or recording is active). Reading always runs, even without a recording: if
 * nobody reads, every output of the ESP32 slows its DSP loop (500 ms TX timeout).
 */
object RadarController {
    private const val TAG = "RadarController"
    private const val ACTION_USB_PERMISSION = "io.github.juvenile454.radarsensor.USB_PERMISSION"

    /** Commands that start a stream (see the firmware help). */
    private val STREAM_CMD = Regex("""^\s*(iq3|doppler|sess\s+run)\b""")

    val store = LiveStore()

    private val _state = MutableStateFlow<ConnState>(ConnState.Idle)
    val state: StateFlow<ConnState> = _state.asStateFlow()

    private val _recording = MutableStateFlow<File?>(null)
    val recording: StateFlow<File?> = _recording.asStateFlow()

    private val _stats = MutableStateFlow(LinkStats())
    val stats: StateFlow<LinkStats> = _stats.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var app: Context

    @Volatile private var transport: Transport? = null

    @Volatile private var recorder: Recorder? = null

    /** Reconnect over USB automatically after unplugging and replugging (until the user disconnects). */
    @Volatile var autoUsb = false
        private set

    private val bytes = AtomicLong()
    private val lines = AtomicLong()
    private var connectedSince = 0L
    private var lastLabel: String? = null

    /**
     * Most recently started stream (iq3/doppler/sess run) as long as it has not ended. The ESP32 runs on phone power:
     * unplugging restarts it, and the stream is gone afterwards. When the same source reconnects, it is started again.
     */
    @Volatile private var activeStream: String? = null

    val recordingsDir: File get() = File(app.getExternalFilesDir(null), "recordings").apply { mkdirs() }

    fun init(context: Context) {
        if (::app.isInitialized) return
        app = context.applicationContext
        val filter =
            IntentFilter().apply {
                addAction(ACTION_USB_PERMISSION)
                addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            }
        app.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        scope.launch { rateLoop() }
    }

    // ---------------------------------------------------------------- Connection

    fun connect(t: Transport) {
        if (t.kind != "usb") autoUsb = false
        disconnectInternal(userInitiated = false)
        // different source: clear display and counters; same source (USB replugged): keep counters across the session
        val sameSource = t.label == lastLabel
        if (!sameSource) {
            store.clear()
            activeStream = null
        }
        lastLabel = t.label
        _state.value = ConnState.Connecting(t.label)
        store.note("verbinde: ${t.label}")
        // set immediately: prevents opening twice (attach broadcast and activity start arrive almost simultaneously) and
        // must come before start() because onClosed() can already arrive during start() (short replay)
        transport = t
        scope.launch {
            val assembler = LineAssembler { onLine(it) }
            try {
                t.start(
                    object : Transport.Listener {
                        override fun onData(
                            data: ByteArray,
                            len: Int,
                        ) {
                            bytes.addAndGet(len.toLong())
                            assembler.feed(data, len)
                        }

                        override fun onClosed(error: Exception?) = onTransportClosed(t, error)
                    },
                )
            } catch (e: Exception) {
                Log.w(TAG, "connect", e)
                if (transport === t) transport = null
                t.close()
                _state.value = ConnState.Failed("${t.label}: ${e.message ?: e.javaClass.simpleName}")
                store.note("Verbindung fehlgeschlagen: ${e.message}")
                return@launch
            }
            if (transport !== t) return@launch // already finished again
            connectedSince = SystemClock.elapsedRealtime()
            _state.value = ConnState.Connected(t.label, t.kind)
            RadarService.update(app)
            if (t.kind != "replay") {
                delay(300)
                send("hello")
                val resume = activeStream
                if (sameSource && resume != null) {
                    delay(700)
                    store.note("Stream nach Neuverbinden wieder gestartet: $resume")
                    send(resume)
                }
            }
        }
    }

    /** By the user: disconnect and do not reconnect over USB automatically. */
    fun disconnect() {
        activeStream = null
        disconnectInternal(userInitiated = true)
        if (_state.value !is ConnState.Connected) _state.value = ConnState.Idle
        RadarService.update(app)
    }

    private fun disconnectInternal(userInitiated: Boolean) {
        if (userInitiated) autoUsb = false
        val t = transport ?: return
        transport = null
        t.close()
        _state.value = ConnState.Idle
        store.note("getrennt: ${t.label}")
        RadarService.update(app)
    }

    private fun onTransportClosed(
        t: Transport,
        error: Exception?,
    ) {
        if (transport !== t) return // already replaced or disconnected by the user
        transport = null
        t.close()
        _state.value =
            when {
                error == null -> ConnState.Ended(t.label)
                t.kind == "usb" && autoUsb -> ConnState.Failed("USB getrennt (${error.message}) – verbinde neu, sobald eingesteckt")
                else -> ConnState.Failed("${t.label}: ${error.message ?: error.javaClass.simpleName}")
            }
        store.note(if (error == null) "Ende: ${t.label}" else "Verbindung verloren: ${error.message}")
        RadarService.update(app)
    }

    val isConnected: Boolean get() = transport != null

    // ---------------------------------------------------------------- Commands

    /** Command line to the ESP32 (line ending is appended). */
    fun send(cmd: String) {
        if (STREAM_CMD.containsMatchIn(cmd)) activeStream = cmd
        sendBytes((cmd + "\n").toByteArray(), cmd)
    }

    /**
     * End the stream: any character ends iq3/doppler/sess run. A single `\n` does no harm if no stream is
     * running (empty line → help, which only ends up in the console).
     */
    fun stopStream() {
        activeStream = null
        sendBytes(byteArrayOf('\n'.code.toByte()), "(Stopp)")
    }

    /** Ctrl+R during iq3/doppler: restart the server on purpose, tests the firmware's reconnect. */
    fun testServerReset() = sendBytes(byteArrayOf(0x12), "(Server-Neustart-Test)")

    private fun sendBytes(
        b: ByteArray,
        label: String,
    ) {
        val t = transport
        if (t == null) {
            store.note("nicht verbunden: $label")
            return
        }
        scope.launch {
            try {
                t.write(b)
                store.note(label)
                recorder?.command(label, SystemClock.elapsedRealtimeNanos())
            } catch (e: Exception) {
                store.note("Senden fehlgeschlagen: ${e.message}")
            }
        }
    }

    // ---------------------------------------------------------------- Receive

    private fun onLine(line: String) {
        lines.incrementAndGet()
        recorder?.line(line, SystemClock.elapsedRealtimeNanos())
        val m = LineParser.parse(line)
        if (m is End || m is SessEnd) activeStream = null // stream ended normally (fixed duration or stop)
        store.apply(line, m)
        dspExec.execute { phoneDsp.offer(line) }
    }

    private suspend fun rateLoop() {
        var lastB = 0L
        var lastL = 0L
        var lastT = SystemClock.elapsedRealtimeNanos()
        while (true) {
            delay(1000)
            val now = SystemClock.elapsedRealtimeNanos()
            val b = bytes.get()
            val l = lines.get()
            val dt = (now - lastT) / 1e9f
            _stats.value = LinkStats((b - lastB) / dt, (l - lastL) / dt, b, connectedSince)
            dspExec.execute {
                phoneDsp.sampleCpu(SystemClock.elapsedRealtimeNanos())
                store.phoneCpu(phoneDsp.cpuPct)
            }
            lastB = b
            lastL = l
            lastT = now
        }
    }

    // ---------------------------------------------------------------- Recording

    fun startRecording(label: String) {
        if (recorder != null) return
        val st = _state.value
        val tr = transport
        val r =
            Recorder.start(
                root = recordingsDir,
                label = label,
                app = "${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME}",
                device = "${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
                transport = tr?.label ?: (st as? ConnState.Connected)?.label ?: "-",
                transportKind = tr?.kind ?: "-",
                hello = store.snapshot().hello?.let { h -> "@HELLO," + h.fields.entries.joinToString(",") { "${it.key}=${it.value}" } },
                nowNanos = SystemClock.elapsedRealtimeNanos(),
            )
        recorder = r
        _recording.value = r.dir
        store.note("Aufnahme: ${r.dir.name}")
        RadarService.update(app)
    }

    fun stopRecording() {
        val r = recorder ?: return
        recorder = null
        r.stop(SystemClock.elapsedRealtimeNanos())
        _recording.value = null
        store.note("Aufnahme gespeichert: ${r.dir.name} (${r.lines} Zeilen)")
        RadarService.update(app)
    }

    val recordingLines: Long get() = recorder?.lines ?: 0

    // ---------------------------------------------------------------- ECG chest strap (V0.1), markers and metronome (V0.2)

    /** ECG chest strap over BLE; every notification goes into the recording (ref_hr.csv) and into the display as a reference curve. */
    val hrRef: HrReference by lazy { HrReference(app, ::onReference) { store.note(it) } }

    private fun onReference(
        rxNanos: Long,
        m: HrMeasurement,
    ) {
        recorder?.reference(rxNanos, m)
        store.reference(m)
    }

    /** Marker in events.csv (only during a recording) and as an event in the display. */
    fun mark(text: String) {
        val now = SystemClock.elapsedRealtimeNanos()
        val r = recorder
        r?.mark(now, text)
        store.note(if (r != null) "Marker: $text" else "Marker ohne Aufnahme: $text")
    }

    /** Metronome breathing: every inhale tone writes "ein" (without an event in the display, that would be too frequent). */
    val metronome: Metronome by lazy { Metronome(scope) { nanos -> recorder?.mark(nanos, "ein") } }

    // ---------------------------------------------------------------- Replay DSP (A5)

    private val _dspStatus = MutableStateFlow<String?>(null)
    val dspStatus: StateFlow<String?> = _dspStatus.asStateFlow()

    /** Recompute an iq3-raw recording on the phone with the firmware DSP and compare it with the ESP32 output. */
    fun replayDsp(dir: File) {
        if (_dspStatus.value?.startsWith("rechnet") == true) return
        _dspStatus.value = "rechnet: ${dir.name}"
        scope.launch(Dispatchers.Default) {
            _dspStatus.value =
                try {
                    val r = DspReplay.run(dir)
                    "${dir.name}: ${r.summary}, ${r.ms} ms"
                } catch (e: Throwable) {
                    Log.w(TAG, "DSP nachrechnen", e)
                    "${dir.name}: Fehler ${e.message}"
                }
            store.note("DSP nachgerechnet: ${_dspStatus.value}")
        }
    }

    // ---------------------------------------------------------------- Phone DSP live (V5.2)

    /** Own thread: receiving does not wait for the computation, line order is preserved. */
    private val dspExec = Executors.newSingleThreadExecutor { r -> Thread(r, "handy-dsp").apply { isDaemon = true } }

    /** Live DSP from `@RAW` (only with `iq3 … raw`), results as a second series next to the ESP32 values. */
    private val phoneDsp = PhoneDsp(LiveDsp) { line -> store.phone(LineParser.parse(line)) }

    private val _phoneDspOn = MutableStateFlow(false)
    val phoneDspOn: StateFlow<Boolean> = _phoneDspOn.asStateFlow()

    /** Why the library is missing (null = loaded). */
    val phoneDspError: String? get() = LiveDsp.loadError

    /** On: computes from now on (mid-stream, with its start and zoom lines), off: no computation load. */
    fun setPhoneDsp(on: Boolean) {
        dspExec.execute {
            phoneDsp.setEnabled(on)
            _phoneDspOn.value = phoneDsp.enabled
            store.phoneCpu(phoneDsp.cpuPct)
        }
        store.note(
            when {
                !on -> "Handy-DSP aus"
                phoneDspError != null -> "Handy-DSP nicht verfügbar: $phoneDspError"
                else -> "Handy-DSP ein (Viterbi + Oberwellen-Abzug, braucht iq3 … raw)"
            },
        )
    }

    // ---------------------------------------------------------------- USB

    /** Connect the first serial USB device; requests permission if needed. */
    fun connectUsb() {
        autoUsb = true
        if (transport?.kind == "usb") return // already connected or being set up
        val usb = app.getSystemService(UsbManager::class.java)
        val driver = UsbSerialTransport.findDrivers(usb).firstOrNull()
        if (driver == null) {
            _state.value = ConnState.Failed("kein serielles USB-Gerät gefunden (ESP32 am USB-Port eingesteckt?)")
            return
        }
        if (!usb.hasPermission(driver.device)) {
            store.note("frage USB-Berechtigung an: ${UsbSerialTransport.describe(driver.device)}")
            _state.value = ConnState.Connecting("USB-Berechtigung …")
            val pi =
                PendingIntent.getBroadcast(
                    app,
                    0,
                    Intent(ACTION_USB_PERMISSION).setPackage(app.packageName),
                    PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                )
            usb.requestPermission(driver.device, pi)
            return
        }
        connect(UsbSerialTransport(usb, driver))
    }

    /** From the activity's USB_DEVICE_ATTACHED start (permission is granted then if the user confirmed). */
    fun onUsbAttachedIntent() {
        if (transport == null) connectUsb()
    }

    private val usbReceiver =
        object : BroadcastReceiver() {
            override fun onReceive(
                context: Context,
                intent: Intent,
            ) {
                val dev = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                when (intent.action) {
                    ACTION_USB_PERMISSION -> {
                        val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                        store.note("USB-Berechtigung ${if (granted) "erteilt" else "verweigert"}")
                        if (granted) connectUsb() else _state.value = ConnState.Failed("USB-Berechtigung verweigert")
                    }
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                        store.note("USB eingesteckt: ${dev?.let { UsbSerialTransport.describe(it) }}")
                        // With "Always open" the system grants the permission only at activity start, shortly after this
                        // broadcast; without a pause an unnecessary permission dialog would appear.
                        if (autoUsb && transport == null) {
                            scope.launch {
                                delay(1500)
                                if (autoUsb && transport == null) connectUsb()
                            }
                        }
                    }
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                        store.note("USB abgezogen: ${dev?.let { UsbSerialTransport.describe(it) }}")
                        val t = transport
                        if (t != null && t.kind == "usb") onTransportClosed(t, java.io.IOException("USB-Gerät abgezogen"))
                    }
                }
            }
        }
}
