package io.github.juvenile454.radarsensor.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import io.github.juvenile454.radarsensor.core.RadarController

/**
 * Debug build only. Control via adb, also with the screen off:
 *
 *   adb shell am broadcast -a io.github.juvenile454.radarsensor.DEBUG -n io.github.juvenile454.radarsensor/.debug.DebugReceiver --es cmd status
 *   … --es cmd rec_start --es name usb_lang | rec_stop | send --es line "iq3 0 raw" | stop | usb | disconnect
 *   … --es cmd ref_scan | ref_connect --es addr <mac> | ref_disconnect | ref_status        (ECG chest strap, V0.1)
 *   … --es cmd mark --es text anhalten_start | metro --ei rate 15 (0 = off)                 (markers, metronome, V0.2)
 *   … --es cmd phone_dsp --ez on true | phone_dsp_status                         (phone DSP live from @RAW, V5.2)
 *
 * The response is in the result of `am broadcast` (data="…").
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
                        in io.github.juvenile454.radarsensor.record.MetronomeSchedule.RATES -> {
                            c.metronome.start(rate)
                            "Metronom $rate /min"
                        }
                        else -> "Takt nur ${io.github.juvenile454.radarsensor.record.MetronomeSchedule.RATES} /min"
                    }
                }
                "phone_dsp" -> {
                    val on = intent.getBooleanExtra("on", true)
                    c.setPhoneDsp(on)
                    if (on && c.phoneDspError != null) "Handy-DSP nicht verfügbar: ${c.phoneDspError}" else "Handy-DSP ${if (on) "ein" else "aus"}"
                }
                "phone_dsp_status" -> {
                    val s = c.store.snapshot()
                    "Handy-DSP ${if (c.phoneDspOn.value) "ein" else "aus"} | CPU ${s.phoneCpuPct?.let { "%.2f %%".format(it) } ?: "-"} | " +
                        "Werte ${s.phoneT.size} | letzter ${s.lastPhoneVital} | Zustand ${s.lastPhoneState?.let { io.github.juvenile454.radarsensor.protocol.MeasState.name(it.state) } ?: "-"} | " +
                        "ESP32 ${s.lastVital?.hrFusedBpm ?: "-"} | Bibliothek ${c.phoneDspError ?: "geladen"}"
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
