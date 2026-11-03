package de.timo.radarsensor.live

/** Fixed-size ring buffer for measurement series. Not thread-safe (access only under the lock of [LiveStore]). */
class FloatRing(
    val capacity: Int,
) {
    private val data = FloatArray(capacity)
    private var head = 0

    var size = 0
        private set

    fun add(v: Float) {
        data[head] = v
        head = (head + 1) % capacity
        if (size < capacity) size++
    }

    fun last(): Float = if (size == 0) Float.NaN else data[(head - 1 + capacity) % capacity]

    /** Oldest first */
    fun toArray(): FloatArray {
        val out = FloatArray(size)
        val start = (head - size + capacity) % capacity
        for (i in 0 until size) out[i] = data[(start + i) % capacity]
        return out
    }

    fun clear() {
        head = 0
        size = 0
    }
}

/**
 * Continuous time axis in seconds from the sensor `tick` (ms). The tick keeps running across DSP compute pauses, in which
 * the frames arrive bundled. After `@RECONNECT` (server restarted) it starts over; then it is joined to the
 * ESP32 time `t_ms` (like `parse_log()` in host/a121_esp_ultimate.py).
 */
class TickClock {
    private var tick0 = -1L
    private var tMs0 = 0L
    private var lastTick = -1L

    fun reset() {
        tick0 = -1L
        lastTick = -1L
    }

    fun seconds(
        tick: Long?,
        tMs: Long,
    ): Float {
        if (tick == null) return tMs / 1000f
        if (tick0 < 0 || (lastTick >= 0 && tick < lastTick)) {
            tick0 = tick
            tMs0 = tMs
        }
        lastTick = tick
        return (tick - tick0 + tMs0) / 1000f
    }
}
