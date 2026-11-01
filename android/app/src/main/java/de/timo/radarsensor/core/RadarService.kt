package de.timo.radarsensor.core

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import de.timo.radarsensor.R
import de.timo.radarsensor.ui.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * Hält den Prozess im Vordergrund (Typ connectedDevice) und die CPU wach, solange eine Verbindung oder Aufnahme
 * läuft. Sonst würde Samsung die App bei ausgeschaltetem Bildschirm einfrieren, der Lese-Thread stünde, und der
 * ESP32 bliebe an seinem USB-Sendepuffer hängen.
 */
class RadarService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var job: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Radar-Verbindung", NotificationManager.IMPORTANCE_LOW))
        startForeground(NOTIF_ID, buildNotification("startet …"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        wakeLock =
            getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "radarsensor:stream")
                .apply { acquire() }
        job =
            scope.launch {
                combine(RadarController.state, RadarController.recording) { s, r -> s to r }.collect { (s, r) ->
                    val text =
                        when (s) {
                            is ConnState.Connected -> "Verbunden: ${s.label}"
                            is ConnState.Connecting -> "Verbinde: ${s.label}"
                            is ConnState.Failed -> "Getrennt: ${s.message}"
                            is ConnState.Ended -> "Beendet: ${s.label}"
                            ConnState.Idle -> "Getrennt"
                        } + if (r != null) " · Aufnahme ${r.name}" else ""
                    nm.notify(NOTIF_ID, buildNotification(text))
                }
            }
    }

    override fun onStartCommand(
        intent: Intent?,
        flags: Int,
        startId: Int,
    ): Int {
        if (!shouldRun()) stopSelf()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        job?.cancel()
        scope.cancel()
        wakeLock?.takeIf { it.isHeld }?.release()
        super.onDestroy()
    }

    private fun buildNotification(text: String): Notification {
        val open =
            PendingIntent.getActivity(
                this,
                0,
                Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE,
            )
        return Notification
            .Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_radar)
            .setContentTitle("Radar-Sensor")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "radar"
        private const val NOTIF_ID = 1

        /** Verbunden, Aufnahme läuft, oder USB abgezogen und Neuverbinden aktiv (Start aus dem Hintergrund wäre verboten). */
        private fun shouldRun() = RadarController.isConnected || RadarController.recording.value != null || RadarController.autoUsb

        /** Startet oder beendet den Dienst passend zum Zustand. */
        fun update(ctx: Context) {
            val i = Intent(ctx, RadarService::class.java)
            if (shouldRun()) {
                try {
                    ctx.startForegroundService(i)
                } catch (e: Exception) {
                    // Start aus dem Hintergrund verboten (z. B. Wiederverbinden ohne sichtbare App): Lesen läuft trotzdem
                    android.util.Log.w("RadarService", "startForegroundService", e)
                }
            } else {
                ctx.stopService(i)
            }
        }
    }
}
