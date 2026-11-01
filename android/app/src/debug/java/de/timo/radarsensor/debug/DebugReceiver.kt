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
