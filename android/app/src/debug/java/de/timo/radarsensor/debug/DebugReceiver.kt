package de.timo.radarsensor.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import de.timo.radarsensor.core.RadarController

/**
 * Nur Debug-Build. Steuerung per adb, auch bei ausgeschaltetem Bildschirm:
 *
 *   adb shell am broadcast -a de.timo.radarsensor.DEBUG -n de.timo.radarsensor/.debug.DebugReceiver --es cmd status
 *   … --es cmd rec_start --es name usb_lang | rec_stop | send --es line "iq3 0 raw" | stop | usb | disconnect
 *   … --es cmd ref_scan | ref_connect --es addr <mac> | ref_disconnect | ref_status        (EKG-Gurt, V0.1)
 *   … --es cmd mark --es text anhalten_start | metro --ei rate 15 (0 = aus)                 (Marker, Metronom, V0.2)
 *
 * Antwort steht im Ergebnis von `am broadcast` (data="…").
 */
class DebugReceiver : BroadcastReceiver() {
    override fun onReceive(
        context: Context,
        intent: Intent,
    ) {
        RadarController.init(context)
        val c = RadarController
        val reply =
            when (val cmd = intent.getStringExtra("cmd")) {
                "rec_start" -> {
                    c.startRecording(intent.getStringExtra("name") ?: "")
                    "Aufnahme: ${c.recording.value?.name}"
                }
                "rec_stop" -> {
                    val d = c.recording.value
                    c.stopRecording()
                    "gespeichert: ${d?.name}"
                }
                "send" -> {
                    val line = intent.getStringExtra("line") ?: ""
                    c.send(line)
                    "gesendet: $line"
                }
                "stop" -> {
                    c.stopStream()
                    "Stopp gesendet"
                }
                "usb" -> {
                    c.connectUsb()
                    "USB verbinden"
                }
                "disconnect" -> {
                    c.disconnect()
                    "getrennt"
                }
                "dsp_replay" -> {
                    val dir = java.io.File(c.recordingsDir, intent.getStringExtra("name") ?: "")
                    if (java.io.File(dir, "stream.log").exists()) {
                        c.replayDsp(dir)
                        "DSP nachrechnen: ${dir.name}"
                    } else {
                        "keine Aufnahme: ${dir.name}"
                    }
                }
                "ref_scan" -> {
                    c.hrRef.scan()
                    "Gurt-Suche 10 s; danach ref_status"
                }
                "ref_connect" -> {
                    val addr = intent.getStringExtra("addr") ?: ""
                    c.hrRef.connect(addr)
                    "verbinde Gurt $addr"
                }
                "ref_disconnect" -> {
                    c.hrRef.disconnect()
                    "Gurt getrennt"
                }
                "ref_status" -> {
                    val r = c.hrRef
                    "Gurt ${r.state.value} | letzter ${r.last} | Benachrichtigungen ${r.notifications} | gefunden " +
                        r.found.value.joinToString { "${it.name ?: "?"} ${it.address} ${it.rssi} dBm" }
                }
                "mark" -> {
                    val text = intent.getStringExtra("text") ?: ""
                    c.mark(text)
                    "Marker $text"
                }
                "metro" -> {
                    val rate = intent.getIntExtra("rate", 0)
                    when (rate) {
                        0 -> {
                            c.metronome.stop()
                            "Metronom aus"
                        }
                        in de.timo.radarsensor.record.MetronomeSchedule.RATES -> {
                            c.metronome.start(rate)
                            "Metronom $rate /min"
                        }
                        else -> "Takt nur ${de.timo.radarsensor.record.MetronomeSchedule.RATES} /min"
                    }
                }
                "status", null -> ""
                else -> "unbekannt: $cmd"
            }
        val s = c.store.snapshot()
        val st = c.stats.value
        resultData =
            "$reply | Zustand ${c.state.value} | Aufnahme ${c.recording.value?.name ?: "-"} (${c.recordingLines} Zeilen) | " +
            "DSP ${c.dspStatus.value ?: "-"} | Modus ${s.mode.label} | %.1f kB/s | Frameverlust ${s.framesLost} in ${s.frameGaps} Lücken | neu verbunden ${s.reconnects} | Zeilen ${s.lines}".format(st.bytesPerS / 1000f)
    }
}
