package de.timo.radarsensor.core

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
import de.timo.radarsensor.BuildConfig
import de.timo.radarsensor.dsp.DspReplay
import de.timo.radarsensor.dsp.LiveDsp
import de.timo.radarsensor.dsp.PhoneDsp
import de.timo.radarsensor.live.LiveStore
import de.timo.radarsensor.protocol.End
import de.timo.radarsensor.protocol.LineAssembler
import de.timo.radarsensor.protocol.LineParser
import de.timo.radarsensor.protocol.SessEnd
import de.timo.radarsensor.record.Recorder
import de.timo.radarsensor.reference.HrMeasurement
import de.timo.radarsensor.reference.HrReference
import de.timo.radarsensor.transport.Transport
import de.timo.radarsensor.transport.UsbSerialTransport
import de.timo.radarsensor.ui.Metronome
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

    /** Gegenstelle hat ohne Fehler beendet (Ende der Wiedergabe, Bridge beendet) */
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
 * Verbindung, Datenfluss und Aufnahme, unabhängig von der Oberfläche (lebt im Prozess; der [RadarService] hält ihn
 * im Vordergrund, solange eine Verbindung oder Aufnahme läuft). Das Lesen läuft immer, auch ohne Aufnahme: Liest
 * niemand, bremst jede Ausgabe des ESP32 seine DSP-Schleife (500 ms TX-Timeout).
 */
object RadarController {
    private const val TAG = "RadarController"
    private const val ACTION_USB_PERMISSION = "de.timo.radarsensor.USB_PERMISSION"

    /** Befehle, die einen Stream starten (siehe Hilfe der Firmware). */
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

    /** Nach Abziehen und Einstecken automatisch wieder per USB verbinden (bis der Nutzer selbst trennt). */
    @Volatile var autoUsb = false
        private set

    private val bytes = AtomicLong()
    private val lines = AtomicLong()
    private var connectedSince = 0L
    private var lastLabel: String? = null

    /**
     * Zuletzt gestarteter Stream (iq3/doppler/sess run), solange er nicht beendet wurde. Der ESP32 hängt am Handy-Strom:
     * Abziehen startet ihn neu, der Stream ist danach weg. Beim Neuverbinden derselben Quelle wird er wieder gestartet.
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

    // ---------------------------------------------------------------- Verbindung

    fun connect(t: Transport) {
        if (t.kind != "usb") autoUsb = false
        disconnectInternal(userInitiated = false)
        // andere Quelle: Anzeige und Zähler leeren; dieselbe Quelle (USB neu eingesteckt): Zähler über die Sitzung behalten
        val sameSource = t.label == lastLabel
        if (!sameSource) {
            store.clear()
            activeStream = null
        }
        lastLabel = t.label
        _state.value = ConnState.Connecting(t.label)
        store.note("verbinde: ${t.label}")
        // sofort setzen: verhindert doppeltes Öffnen (Attach-Broadcast und Activity-Start kommen fast gleichzeitig) und
        // muss vor start() stehen, weil onClosed() schon während start() kommen kann (kurze Wiedergabe)
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
            if (transport !== t) return@launch // schon wieder beendet
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

    /** Vom Nutzer: trennen und nicht automatisch wieder per USB verbinden. */
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
        if (transport !== t) return // schon ersetzt oder vom Nutzer getrennt
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

    // ---------------------------------------------------------------- Befehle

    /** Befehlszeile an den ESP32 (Zeilenende wird angehängt). */
    fun send(cmd: String) {
        if (STREAM_CMD.containsMatchIn(cmd)) activeStream = cmd
        sendBytes((cmd + "\n").toByteArray(), cmd)
    }

    /**
     * Stream beenden: Jedes Zeichen beendet iq3/doppler/sess run. Ein einzelnes `\n` stört nicht, falls gerade
     * kein Stream läuft (leere Zeile → Hilfe, die nur in der Konsole landet).
     */
    fun stopStream() {
        activeStream = null
        sendBytes(byteArrayOf('\n'.code.toByte()), "(Stopp)")
    }

    /** Strg+R während iq3/doppler: Server absichtlich neu starten, prüft das Neuverbinden der Firmware. */
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

    // ---------------------------------------------------------------- Empfang

    private fun onLine(line: String) {
        lines.incrementAndGet()
        recorder?.line(line, SystemClock.elapsedRealtimeNanos())
        val m = LineParser.parse(line)
        if (m is End || m is SessEnd) activeStream = null // Stream regulär zu Ende (feste Dauer oder Stopp)
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

    // ---------------------------------------------------------------- Aufnahme

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

    // ---------------------------------------------------------------- EKG-Gurt (V0.1), Marker und Metronom (V0.2)

    /** EKG-Gurt über BLE; jede Benachrichtigung in die Aufnahme (ref_hr.csv) und als Referenzkurve in die Anzeige. */
    val hrRef: HrReference by lazy { HrReference(app, ::onReference) { store.note(it) } }

    private fun onReference(
        rxNanos: Long,
        m: HrMeasurement,
    ) {
        recorder?.reference(rxNanos, m)
        store.reference(m)
    }

    /** Marker in events.csv (nur während einer Aufnahme) und als Ereignis in der Anzeige. */
    fun mark(text: String) {
        val now = SystemClock.elapsedRealtimeNanos()
        val r = recorder
        r?.mark(now, text)
        store.note(if (r != null) "Marker: $text" else "Marker ohne Aufnahme: $text")
    }

    /** Metronom-Atmung: jeder Einatem-Ton schreibt „ein" (ohne Ereignis in der Anzeige, das wäre zu oft). */
    val metronome: Metronome by lazy { Metronome(scope) { nanos -> recorder?.mark(nanos, "ein") } }

    // ---------------------------------------------------------------- DSP nachrechnen (A5)

    private val _dspStatus = MutableStateFlow<String?>(null)
    val dspStatus: StateFlow<String?> = _dspStatus.asStateFlow()

    /** iq3-raw-Aufnahme am Handy mit dem Firmware-DSP neu rechnen und mit der ESP32-Ausgabe vergleichen. */
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

    // ---------------------------------------------------------------- Handy-DSP live (V5.2)

    /** Eigener Thread: Empfang wartet nicht auf die Rechnung, Reihenfolge der Zeilen bleibt. */
    private val dspExec = Executors.newSingleThreadExecutor { r -> Thread(r, "handy-dsp").apply { isDaemon = true } }

    /** Live-DSP aus `@RAW` (nur bei `iq3 … raw`), Ergebnisse als zweite Reihe neben den ESP32-Werten. */
    private val phoneDsp = PhoneDsp(LiveDsp) { line -> store.phone(LineParser.parse(line)) }

    private val _phoneDspOn = MutableStateFlow(false)
    val phoneDspOn: StateFlow<Boolean> = _phoneDspOn.asStateFlow()

    /** Warum die Bibliothek fehlt (null = geladen). */
    val phoneDspError: String? get() = LiveDsp.loadError

    /** Ein: rechnet ab sofort mit (mitten im Stream mit dessen Start- und Zoom-Zeile), aus: keine Rechenlast. */
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

    /** Erstes serielles USB-Gerät verbinden; fragt bei Bedarf die Berechtigung an. */
    fun connectUsb() {
        autoUsb = true
        if (transport?.kind == "usb") return // schon verbunden oder im Aufbau
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

    /** Vom USB_DEVICE_ATTACHED-Start der Activity (Berechtigung ist dann erteilt, wenn der Nutzer bestätigt hat). */
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
                        // Mit „Immer öffnen“ erteilt das System die Berechtigung erst beim Activity-Start, kurz nach diesem
                        // Broadcast; ohne Pause käme ein unnötiger Berechtigungsdialog.
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
