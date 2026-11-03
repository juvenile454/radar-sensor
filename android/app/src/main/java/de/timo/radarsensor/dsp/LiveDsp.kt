package de.timo.radarsensor.dsp

/** Compute core of the phone DSP; [LiveDsp] is the native version, tests substitute a replacement. */
interface DspEngine {
    val available: Boolean

    fun reset()

    /** One received line; returns the phone lines completed by it (separated by '\n') or null. */
    fun line(line: String): String?

    /** CPU time of all [line] calls in ns. */
    fun cpuNanos(): Long
}

/**
 * V5.2: live DSP on the phone from `@RAW` (NDK library radardsp_live, `host/dsp_replay/dsp_live.h`): the same
 * firmware header as on the ESP32, but with `A121_DSP_HEAVY` (Viterbi tracking and subtraction of breathing harmonics).
 * Computes at the same points as the ESP32 (at its `@DSP` lines), timestamps from the ESP32. If the library is missing,
 * [available] is false and [loadError] says why.
 */
object LiveDsp : DspEngine {
    val loadError: String? =
        try {
            System.loadLibrary("radardsp_live")
            null
        } catch (e: UnsatisfiedLinkError) {
            e.message ?: "radardsp_live nicht ladbar"
        }

    override val available: Boolean get() = loadError == null

    private external fun nativeReset()

    private external fun nativeLine(line: String): String?

    private external fun nativeCpuNanos(): Long

    override fun reset() = nativeReset()

    override fun line(line: String): String? = nativeLine(line)

    override fun cpuNanos(): Long = nativeCpuNanos()
}
