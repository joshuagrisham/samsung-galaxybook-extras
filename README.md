# Samsung Galaxy Book Extras

The [samsung-galaxybook](https://docs.kernel.org/admin-guide/laptops/samsung-galaxybook.html) x86 Platform driver has now been moved to Linux Mainline starting with version 6.15.

- Kernel documentation page: [Samsung Galaxy Book Driver](https://docs.kernel.org/admin-guide/laptops/samsung-galaxybook.html)
- Mainline source: [drivers/platform/x86/samsung-galaxybook.c](https://web.git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/platform/x86/samsung-galaxybook.c)

The module can be used with any kernel version starting 6.14.0 and higher. If you build it yourself, you will need to ensure that all required dependencies are selected; see [SAMSUNG_GALAXYBOOK config](https://web.git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/platform/x86/Kconfig?id=7cdabafc001202de9984f22c973305f424e0a8b7#n792) for details (at least as of 6.15-rc1) and can build the driver either in-tree of your 6.14+ source or out-of-tree using the approperiate headers.

Should you wish to use the driver with a kernel version <= `6.13.x`, there is a [pre-6.14](https://github.com/joshuagrisham/samsung-galaxybook-extras/tree/pre-6.14) branch in this repository which provides the latest copy of the original protytope and additional instructions for how to build and use it.

## Reporting issues

Additional changes are not planned to the driver from this repository, but instead I will focus only on adding additional devices and fixing issues in the mainline driver.

You are welcome to create an issue in this repository with a problem or just a general question if you like and I will try to respond whenever I am able to do so; otherwise, the official channels can be used: for bugs with the platform driver itself, submit them in [Bugzilla](https://bugzilla.kernel.org/) under the product [Platform Specific/Hardware](https://bugzilla.kernel.org/describecomponents.cgi?product=Platform%20Specific%2FHardware) and component [x86-64](https://bugzilla.kernel.org/buglist.cgi?component=x86-64&product=Platform%20Specific%2FHardware&resolution=---). You can also utilize the bug-reporting services provided by your distrubtion of choice, as well as turn to the [platform-driver-x86 mailing list](https://lore.kernel.org/platform-driver-x86/) for any detailed technical questions and support.

## Additional features not covered by this driver

### Keyboard hotkey issues

Most of the relevant keyboard hotkeys should now be handled by the platform driver itself (see the above-linked driver documentation for more information on this).

Several of the other keyboard hotkey events have various "issues"; I have submitted a patch to help filter out these problematic keys as part of [systemd's 60-keyboard.hwdb](https://github.com/systemd/systemd/blob/be1f90d97fb1295247aed6bd1286ebcd42408c30/hwdb.d/60-keyboard.hwdb#L1918-L1929), so as long as you use a recent version of `systemd` which includes these changes then you should hopefully not have these problems (e.g. the Settings key indefinitely spamming `±` (plus-minus sign) after the first time you press it, etc).

Otherwise, you can manually add these rules yourself using the provided [61-keyboard-samsung-galaxybook.hwdb](./61-keyboard-samsung-galaxybook.hwdb) like in the below example:

```sh
sudo cp 61-keyboard-samsung-galaxybook.hwdb /etc/udev/hwdb.d/
sudo systemd-hwdb update
sudo udevadm trigger
```

### Sound from the speakers (enabling speaker amps)

On most of these devices, the speaker amps are not enabled by default and require a quirk which dynamically enables them during audio playback. I have added this as a patch which covers many of the existing 2-speaker and 4-speaker models.

The original patch that solidified current support can be found [here](https://lore.kernel.org/linux-sound/20240909193000.838815-1-josh@joshuagrisham.com/) and additional device IDs have been added to the `alc269_fixup_tbl` quirk table since this patch was introduced. See here for a LOT more information and background to this: https://github.com/thesofproject/linux/issues/4055

If you have a device which does not get any sound through the speakers and your device is not currently part of the quirk table, support can be added by adding your device ID to the quirk table. An easy way to test if your speaker works with the existing 2-amp or 4-amp models is as follows:

```sh
# Force using alc298-samsung-amp-v2-2-amps model
sudo tee /etc/modprobe.d/audio-fix.conf <<< 'options snd-hda-intel model=alc298-samsung-amp-v2-2-amps'

# OR, force using alc298-samsung-amp-v2-4-amps model
sudo tee /etc/modprobe.d/audio-fix.conf <<< 'options snd-hda-intel model=alc298-samsung-amp-v2-4-amps'
```

After setting one of those (choose either the 2 amp or 4 amp version depending on if your device has 2 or 4 speakers; it should not harm to try both if you are not sure!), then you should power off your device completely, wait at least a few seconds, and then power it on again and see if you are getting sound from the speakers.

If the speakers are now working, then your device ID can be added to the quirk table to use that specific model name (feel free to submit an issue here or in Bugzilla under Drivers/Sound if you are unsure how to proceed with this). Once that change is accepted and merged, then you would be able to remove `/etc/modprobe.d/audio-fix.conf`.

If you still do not have sound from the speakers after testing these two models, you can try any of the various troubleshooting techniques mentioned in the above SOF project issue, included the recommendentations from [my own comment 2349301491 from 13 September 2024](https://github.com/thesofproject/linux/issues/4055#issuecomment-2349301491).

### Fan speed reporting

I originally included support for fan speed reporting in this driver itself. Since then, I have instead submitted a patch the ACPI core so that all fans which have the `_FST` method will attempt to report their speed using this method even if all of the other ACPI 4.0 fan methods are missing ([see here for more information](https://lore.kernel.org/linux-acpi/20250222094407.9753-1-josh@joshuagrisham.com/)).

This patch will also come with version 6.15 of the kernel, and works well Samsung Galaxy Book series devices. However, many of these devices have a bug in the BIOS which will cause the `_FST` method to throw an exception and speed will still not be correctly reported by the device. The "bug" is that the method is returning a reference to the location of where the speed value can be fetched, instead of the value itself.

I have tried to report this issue to Samsung and did in fact receive the following response back:

```text
Received: Mon, 20 Jan 2025 18:55:52 +0000 (UTC)

Dear Joshua,

Thank you for bringing this issue to our attention and for your detailed explanation. We appreciate your efforts in identifying and patching the DSDT to address the fan speed bug.

We have forwarded your findings and the suggested BIOS update to our engineering team for further investigation. We will work on resolving this issue and aim to release an update to fix the fan speed bug for the affected devices.
```

As of April 2025 I have not heard anything else nor seen any BIOS updates for my device, so I am not sure exactly what or when we can expect anything. However, should you wish to patch your own DSDT so that it works on your device, you will need to find the `_FST` method of your fan device(s) and potentially wrap the return value in a `DerofOf()` function, then rebuild your DSDT and somehow add it as part of your kernel's boot procedure.

Here is a diff of the fix for my own device (the NP950XED):

```diff
diff --git a/dsdt/NP950XED-dsdt.dsl b/dsdt/NP950XED-dsdt.dsl
index 58cc73d..695ad36 100644
--- a/dsdt/NP950XED-dsdt.dsl
+++ b/dsdt/NP950XED-dsdt.dsl
@@ -82328,17 +82328,17 @@ DefinitionBlock ("", "DSDT", 2, "SECCSD", "LH43STAR", 0x01072009)
                 SFST [One] = Local0
                 If ((Local0 == Zero))
                 {
                     SFST [0x02] = Local0
                 }
                 Else
                 {
                     Local0--
-                    Local1 = FANT [Local0]
+                    Local1 = DerefOf ( FANT [Local0] )
                     Local1 += 0x0A
                     SFST [0x02] = Local1
                 }
 
                 Return (SFST) /* \_SB_.PC00.LPCB.FAN0.SFST */
             }
 
             Method (_DSM, 4, Serialized)  // _DSM: Device-Specific Method
```

For my own testing, I have just built the modified AML file and added it to GRUB in exactly the same way as is documented on [ArchLinux's DSDT wiki page](https://wiki.archlinux.org/title/DSDT#Using_the_AML_with_GRUB).

## Additional debugging

TODO: Add details on how to enable ACPI tracing and/or use eBPF tracing in conjunction with this device. I have tested both options, and both work quite well, though eBPF is a much "cleaner" solution (ACPI debugging can be REALLY verbose!).

This additional debugging information might be necessary for devices which are not working as expected with this driver.
