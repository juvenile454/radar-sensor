package io.github.juvenile454.radarsensor

import android.app.Application
import io.github.juvenile454.radarsensor.core.RadarController

class RadarApp : Application() {
    override fun onCreate() {
        super.onCreate()
        RadarController.init(this)
    }
}
