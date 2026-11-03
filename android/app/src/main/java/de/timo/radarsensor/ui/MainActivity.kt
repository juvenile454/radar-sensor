package de.timo.radarsensor.ui

import android.content.Intent
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryScrollableTabRow
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import de.timo.radarsensor.core.ConnState
import de.timo.radarsensor.core.RadarController
import de.timo.radarsensor.live.LiveSnapshot
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    private val permissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        // Notification (foreground service) and Bluetooth for the ECG chest strap (V0.1)
        val missing =
            listOf(
                android.Manifest.permission.POST_NOTIFICATIONS,
                android.Manifest.permission.BLUETOOTH_SCAN,
                android.Manifest.permission.BLUETOOTH_CONNECT,
            ).filter { checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) permissions.launch(missing.toTypedArray())
        handleIntent(intent)
        setContent { RadarTheme { RadarScreen() } }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(i: Intent?) {
        if (i?.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) RadarController.onUsbAttachedIntent()
    }

    /** Keep the screen on while connected and wanted; recording also runs with the screen off. */
    fun keepScreenOn(on: Boolean) {
        if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }
}

@Composable
fun RadarTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme =
            darkColorScheme(
                primary = Pal.c1,
                secondary = Pal.c2,
                background = Color(0xFF101418),
                surface = Color(0xFF151B21),
            ),
        content = content,
    )
}

enum class Page(
    val title: String,
) {
    OVERVIEW("Übersicht"),
    SIGNAL("Signal"),
    ROOM("Raum"),
    VITAL("Vital"),
    DOPPLER("Doppler"),
    EXPERT("Experten"),
    DIAG("Diagnose"),
    CONSOLE("Konsole"),
    RECORDINGS("Aufnahmen"),
}

@Composable
fun RadarScreen() {
    val state by RadarController.state.collectAsState()
    val recording by RadarController.recording.collectAsState()
    val stats by RadarController.stats.collectAsState()
    var page by rememberSaveable { mutableIntStateOf(0) }
    var snap by remember { mutableStateOf<LiveSnapshot?>(null) }
    var keepOn by rememberSaveable { mutableStateOf(true) }

    // Update the UI at ~12 Hz, only when new data is available
    val lifecycle = androidx.lifecycle.compose.LocalLifecycleOwner.current.lifecycle
    LaunchedEffect(lifecycle) {
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            var last = -1L
            while (true) {
                val v = RadarController.store.version()
                if (v != last) {
                    snap = RadarController.store.snapshot()
                    last = v
                }
                delay(80)
            }
        }
    }
    val activity = androidx.compose.ui.platform.LocalContext.current as? MainActivity
    LaunchedEffect(state, keepOn) { activity?.keepScreenOn(keepOn && state is ConnState.Connected) }

    Scaffold { pad ->
        Column(Modifier.fillMaxSize().padding(pad)) {
            ConnectionBar(state, recording, stats, keepOn) { keepOn = it }
            PrimaryScrollableTabRow(selectedTabIndex = page, edgePadding = 4.dp) {
                Page.entries.forEachIndexed { i, p ->
                    Tab(selected = page == i, onClick = { page = i }, text = { Text(p.title) })
                }
            }
            val s = snap
            if (s == null) {
                Text("…", Modifier.padding(16.dp))
            } else {
                when (Page.entries[page]) {
                    Page.OVERVIEW -> OverviewPage(s, stats)
                    Page.SIGNAL -> SignalPage(s)
                    Page.ROOM -> RoomPage(s)
                    Page.VITAL -> VitalPage(s)
                    Page.DOPPLER -> DopplerPage(s)
                    Page.EXPERT -> ExpertPage(s)
                    Page.DIAG -> DiagPage(s, stats)
                    Page.CONSOLE -> ConsolePage(s)
                    Page.RECORDINGS -> RecordingsPage(recording)
                }
            }
        }
    }
}
