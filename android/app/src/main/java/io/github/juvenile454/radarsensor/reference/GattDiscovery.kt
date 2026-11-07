package io.github.juvenile454.radarsensor.reference

/** = BluetoothGatt.GATT_SUCCESS (here without Android classes, so that the decision stays testable on the PC). */
const val GATT_OK = 0

/** Consecutive failed service discoveries until the chest strap is reported as an error. */
const val MAX_DISCOVERY_FAILURES = 5

enum class Discovery { Subscribe, NoHrService, Reconnect, GiveUp }

/**
 * What happens after onServicesDiscovered. Only a successful discovery without Heart Rate Service means "wrong
 * device"; an error status (e.g. 129/133 after automatic reconnecting) disconnects and reconnects, until
 * [MAX_DISCOVERY_FAILURES] discoveries in a row have failed. failures: failures so far in a row.
 */
fun discoveryAction(
    status: Int,
    hasHr: Boolean,
    failures: Int,
): Discovery =
    when {
        status == GATT_OK && hasHr -> Discovery.Subscribe
        status == GATT_OK -> Discovery.NoHrService
        failures + 1 < MAX_DISCOVERY_FAILURES -> Discovery.Reconnect
        else -> Discovery.GiveUp
    }
