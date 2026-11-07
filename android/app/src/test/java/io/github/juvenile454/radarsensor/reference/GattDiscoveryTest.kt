package io.github.juvenile454.radarsensor.reference

import org.junit.Assert.assertEquals
import org.junit.Test

/** Decision after the service discovery on the chest strap (HrReference.onServicesDiscovered). */
class GattDiscoveryTest {
    @Test
    fun abonniertBeiErfolgMitHeartRate() {
        assertEquals(Discovery.Subscribe, discoveryAction(GATT_OK, hasHr = true, failures = 0))
        assertEquals(Discovery.Subscribe, discoveryAction(GATT_OK, hasHr = true, failures = MAX_DISCOVERY_FAILURES - 1))
    }

    @Test
    fun keinHeartRateNurBeiErfolgreicherSuche() {
        assertEquals(Discovery.NoHrService, discoveryAction(GATT_OK, hasHr = false, failures = 0))
    }

    @Test
    fun fehlerstatusVerbindetWieder() {
        // 129 GATT_INTERNAL_ERROR, 133 GATT_ERROR: typical after automatic reconnecting, no reason to give up
        assertEquals(Discovery.Reconnect, discoveryAction(129, hasHr = false, failures = 0))
        assertEquals(Discovery.Reconnect, discoveryAction(133, hasHr = false, failures = 1))
        assertEquals(Discovery.Reconnect, discoveryAction(129, hasHr = true, failures = 0)) // service possibly from an old cache
    }

    @Test
    fun gibtNachWiederholtenFehlernAuf() {
        assertEquals(Discovery.Reconnect, discoveryAction(133, hasHr = false, failures = MAX_DISCOVERY_FAILURES - 2))
        assertEquals(Discovery.GiveUp, discoveryAction(133, hasHr = false, failures = MAX_DISCOVERY_FAILURES - 1))
    }
}
