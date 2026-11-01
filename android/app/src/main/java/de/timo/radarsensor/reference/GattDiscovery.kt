package de.timo.radarsensor.reference

/** = BluetoothGatt.GATT_SUCCESS (hier ohne Android-Klassen, damit die Entscheidung am PC testbar bleibt). */
const val GATT_OK = 0

/** Aufeinanderfolgende fehlgeschlagene Dienstsuchen, bis der Gurt als Fehler gemeldet wird. */
const val MAX_DISCOVERY_FAILURES = 5

enum class Discovery { Subscribe, NoHrService, Reconnect, GiveUp }

/**
 * Was nach onServicesDiscovered geschieht. Nur eine erfolgreiche Suche ohne Heart Rate Service heißt „falsches
 * Gerät"; ein Fehlerstatus (z. B. 129/133 nach automatischem Wiederverbinden) trennt und verbindet wieder, bis
 * [MAX_DISCOVERY_FAILURES] Suchen in Folge fehlgeschlagen sind. failures: bisherige Fehlschläge in Folge.
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
