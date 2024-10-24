/* Dummy fan devices to test with multiple fans each with various functionality supported */

DefinitionBlock ("", "SSDT", 2, "GBTSTF", "GBKSSDTF", 0x00000001)
{
    Scope(\_SB)
    {
        Device(GBF1) /* Galaxy Book Fan 1 - should be supported out-of-the-box by ACPI */
        {
            Name (_HID, EisaId ("PNP0C0B") /* Fan (Thermal Solution) */)  // _HID: Hardware ID
            Name (_UID, 1)  // _UID: Unique ID
            Name (_STR, Unicode ("GB Test Fan 1"))  // _STR: Description String
            Name (_FIF, Package (0x04)   // _FIF: Fan Information
            {
                0,
                5,
                1,
                0
            })
            Name (_FPS, Package (0x02)   // _FPS: Fan Performance States
            {
                0,
                Package (0x05)
                {
                    1,
                    0,
                    1500,
                    0xFFFFFFFF,
                    0xFFFFFFFF
                }
            })
            Method (_FSL, 1, NotSerialized)  // _FSL: Fan Set Level
            {
            }
            Name (_FST, Package (0x03)   // _FST: Fan Status
            {
                0,
                1,
                1525
            })
        }
        Device(GBF2) /* Galaxy Book Fan 2 - should use _FST and includes _STR */
        {
            Name (_HID, EisaId ("PNP0C0B") /* Fan (Thermal Solution) */)  // _HID: Hardware ID
            Name (_UID, 2)  // _UID: Unique ID
            Name (_STR, Unicode ("GB Test Fan 2"))  // _STR: Description String
            Name (_FST, Package (0x03)   // _FST: Fan Status
            {
                0,
                2,
                2899
            })
        }
        Device(GBF3) /* Galaxy Book Fan 3 - should use _FST and does not include _STR */
        {
            Name (_HID, EisaId ("PNP0C0B") /* Fan (Thermal Solution) */)  // _HID: Hardware ID
            Name (_UID, 3)  // _UID: Unique ID
            Name (_FST, Package (0x03)   // _FST: Fan Status
            {
                0,
                3,
                3421
            })
        }
        Device(GBF4) /* Galaxy Book Fan 4 - does not have _FST or FANT; should not be supported */
        {
            Name (_HID, EisaId ("PNP0C0B") /* Fan (Thermal Solution) */)  // _HID: Hardware ID
            Name (_UID, 4)  // _UID: Unique ID
            Name (FSTX, Package (0x03)   // Non-standard Fan Status
            {
                0,
                4,
                1785
            })
        }
    }
}
