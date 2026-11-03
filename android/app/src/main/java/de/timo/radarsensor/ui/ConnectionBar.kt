package de.timo.radarsensor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.text.KeyboardOptions
import de.timo.radarsensor.BuildConfig
import de.timo.radarsensor.core.ConnState
import de.timo.radarsensor.core.LinkStats
import de.timo.radarsensor.core.RadarController
import de.timo.radarsensor.transport.TcpTransport
import java.io.File

/** Vorbelegung der Entwicklungs-Weiterleitung (host/esp_tcp_bridge.py), z. B. der PC im Tailnet; aus
 *  android/local.properties `radar.bridge=<host>:7777` (nicht im Repo), sonst leer. */
val DEFAULT_BRIDGE: String = BuildConfig.DEFAULT_BRIDGE

@Composable
fun ConnectionBar(
    state: ConnState,
    recording: File?,
    stats: LinkStats,
    keepOn: Boolean,
    onKeepOn: (Boolean) -> Unit,
) {
    var expanded by rememberSaveable { mutableStateOf(false) }
    var bridge by rememberSaveable { mutableStateOf(DEFAULT_BRIDGE) }
    var label by rememberSaveable { mutableStateOf("") }
    val (dot, text) =
        when (state) {
            is ConnState.Connected -> Pal.ok to state.label
            is ConnState.Connecting -> Pal.warn to "verbinde: ${state.label}"
            is ConnState.Failed -> Pal.bad to state.message
            is ConnState.Ended -> Pal.axis to "beendet: ${state.label}"
            ConnState.Idle -> Pal.axis to "nicht verbunden"
        }
    Column(Modifier.fillMaxWidth().background(Color(0xFF151B21)).padding(horizontal = 8.dp, vertical = 4.dp)) {
        Row(Modifier.fillMaxWidth().clickable { expanded = !expanded }, verticalAlignment = Alignment.CenterVertically) {
            Text("●", color = dot, fontSize = 18.sp)
            Text(" $text", Modifier.weight(1f), fontSize = 13.sp, maxLines = 2)
            if (state is ConnState.Connected) Text("%.1f kB/s ".format(stats.bytesPerS / 1000f), fontSize = 12.sp, color = Pal.axis)
            if (recording != null) Text("● REC", color = Pal.bad, fontSize = 13.sp)
            Text(if (expanded) " ▲" else " ▼", color = Pal.axis)
        }
        if (expanded) {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Button(onClick = { RadarController.connectUsb() }) { Text("USB") }
                OutlinedButton(onClick = {
                    val h = bridge.substringBeforeLast(':')
                    val p = bridge.substringAfterLast(':').toIntOrNull() ?: 7777
                    RadarController.connect(TcpTransport(h, p))
                }) { Text("TCP") }
                OutlinedButton(onClick = { RadarController.disconnect() }) { Text("Trennen") }
            }
            OutlinedTextField(
                bridge,
                { bridge = it },
                Modifier.fillMaxWidth(),
                label = { Text("Entwicklungs-Weiterleitung host:port") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Switch(keepOn, onKeepOn)
                Text("  Bildschirm an lassen, solange verbunden", fontSize = 13.sp)
            }
        }
        // Aufnahme und Stream-Befehle immer sichtbar
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            if (recording == null) {
                OutlinedTextField(label, { label = it }, Modifier.weight(1f), placeholder = { Text("Name (optional)") }, singleLine = true, textStyle = androidx.compose.ui.text.TextStyle(fontSize = 13.sp))
                Button(
                    onClick = { RadarController.startRecording(label) },
                    colors = ButtonDefaults.buttonColors(containerColor = Pal.bad),
                    modifier = Modifier.padding(start = 6.dp),
                ) { Text("Aufnahme") }
            } else {
                Text("${recording.name}\n${RadarController.recordingLines} Zeilen", Modifier.weight(1f), fontSize = 12.sp)
                Button(onClick = { RadarController.stopRecording() }) { Text("Stopp Aufnahme") }
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Quick("iq3 ∞") { RadarController.send("iq3 0") }
            Quick("iq3 ∞ raw") { RadarController.send("iq3 0 500 raw") }
            Quick("doppler ∞") { RadarController.send("doppler 0") }
            Quick("Stopp", Pal.bad) { RadarController.stopStream() }
        }
    }
}

@Composable
fun Quick(
    text: String,
    color: Color = Pal.c1,
    onClick: () -> Unit,
) {
    OutlinedButton(onClick = onClick, contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 10.dp, vertical = 2.dp)) {
        Text(text, color = color, fontSize = 13.sp)
    }
}

@Composable
fun Labeled(
    label: String,
    value: String,
) {
    Row(Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
        Text(label, Modifier.width(170.dp), fontSize = 13.sp, color = Pal.axis)
        Text(value, fontSize = 13.sp)
    }
}
