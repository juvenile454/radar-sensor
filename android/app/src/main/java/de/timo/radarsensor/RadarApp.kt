package de.timo.radarsensor

import android.app.Application
import de.timo.radarsensor.core.RadarController

class RadarApp : Application() {
    override fun onCreate() {
        super.onCreate()
        RadarController.init(this)
    }
}
