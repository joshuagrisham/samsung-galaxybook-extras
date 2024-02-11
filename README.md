# Samsung Galaxybook Extras ⚠️(WIP)⚠️

Samsung Galaxybook Linux platform driver and accompanying utilities.

Current status: ⚠️ **WIP and dare I say definitely not yet even alpha!** ⚠️ (use at your own risk!)

The intent is to somewhat replicate in Linux what Samsung has done in Windows with what I think the following components are doing:

- **Samsung System Event Controller**: an ACPI driver that interacts with their `SCAI` ACPI device in order to control a lot of these settings.
- **Samsung System Support Engine**: a service which in turn starts the background program `SamsungSystemSupportEngine.exe` which seems to handle quite a lot of things on the userspace/application side, including:
  - setting some last-known values at start time (Performance mode, possibly others?),
  - automatically turning off and on the keyboard backlight based on a configured idle time,
  - displaying OSD notifications upon hotkey presses,
  - etc
- **Samsung Settings**: GUI application to configure all of the different available features.

## Platform Driver

This is a new and (currently) completely out-of-tree kernel platform driver intended to mimic what the **Samsung System Event Controller** Windows system device driver seems to be doing (namely, communicate with the `SCAI` ACPI device in order to control these extra features). Once more features have been added and it has been tested then my intention was to try and submit the driver to be added to the kernel.

`cd` over to the [driver/](./driver/) folder and follow the README there for compiling and installing the module.

The following features are currently implemented:

- An *early* attempt to support hotkey handling (currently supports only keyboard backlight)
- Keyboard backlight
- Battery saver (stop charging at 85%)
- Start device automatically when opening lid
- USB ports provide charging when device is turned off
- Fan speed monitoring via `fan_speed_rpm` sysfs attribute plus a new hwmon device.
- Performance mode (High performance, Optimized, Quiet, Silent) *note: setting is supported and seems to be working, but I have not found a way to read the current value from the device; see below*

The following features might be possible to implement but require  additional debugging and development:

- "Dolby Atmos" mode for the speakers
- Capture input of the Performance mode hotkey (Fn+F11) (it does not come via the main keyboard device nor does it seem to notify the `SCAI` ACPI device)

### General observations

One general observation that I have made is that there are in fact quite a lot of bugs in Samsung's firmware for these devices, for example:

- Exceptions thrown from the firmware itself when certain ACPI methods are executed (both in Windows and in Linux)
- ACPI specification has not been followed 100% in some cases (mismatched method signatures e.g. wrong data types for parameters and return values, missing fields or methods, etc)
- etc

And then that I have seen a bit of "flakiness" from the device when these kind of issues happen. One of the most noticeable is that the keyboard backlight starts to turn off by itself when such problems occur; this needs to be investigated further.

It would be great if we could actually get some help from Samsung regarding this!

### Hotkeys

Samsung have decided to use the main keyboard device to also send most of the hotkey events. If the driver wishes to capture and act on these hotkeys, then we will have to do something like using a i8402 filter to "catch" the key events.

I have also found that some of the hotkey events have conflicts so it is a bit of a tricky territory. As such, this "feature" is in very early stages!

#### Keyboard backlight hotkey

Currently the only key supported is the keyboard backlight key (Fn+F9). The action will be triggered on keyup of the hotkey as the event reported by keydown seems to be the same event for battery charging progress (and thus things get a little crazy when you start charging!).

The hotkey should also trigger the hardware changed event for the LED, which in GNOME automatically displays a nice OSD popup with the correct baclight level displayed.

### Keyboard backlight

A new LED class called `samsung-galaxybook::kbd_backlight` is created which can be controlled via `sysfs` at `/sys/class/leds/samsung-galaxybook::kbd_backlight/brightness` (values 0 to 3) or by many of the standard utilities such as `brightnessctl`, `light`, etc.

It also seems to be picked up automatically in GNOME 45.x in the panel, where you can click the arrow beside `Keyboard` and adjust the slider:

![GNOME Panel Keyboard Backlight](./resources/keyboard-backlight-gnome.png "GNOME Panel Keyboard Backlight")

Note that the setting "automatically turn off the keyboard backlight after X seconds" in Windows is actually controlled by Samsung's application service and not by the device driver itself; if such a feature is desired then it would need to be a similar software-based solution (e.g. added to the "extras" or something).

### Battery saver

To turn on or off the "Battery saver" mode (battery will stop charging at 85%), there is a new device attribute created at `/sys/devices/platform/samsung-galaxybook/battery_saver` which can be read from or written to. A value of 0 means "off" while a value of 1 means "on".

```sh
# read current value (0 for disabled, 1 for enabled)
cat /sys/devices/platform/samsung-galaxybook/battery_saver

# turn on (supports values such as: 1, on, true, yes, etc)
echo true | sudo tee /sys/devices/platform/samsung-galaxybook/battery_saver

# turn off (supports values such as: 0, off, false, no, etc)
echo 0 | sudo tee /sys/devices/platform/samsung-galaxybook/battery_saver
```

> **Note:** I have noticed that if you are currently plugged in with the setting turned on and already sitting at 85%, then disable this setting (with the idea that you wish to charge to 100%), charging does not seem to start automatically. It may be necessary to disconnect and reconnect the charging cable in this case. The Windows driver seems to be doing some hocus pocus with the ACPI battery device that I have not quite sorted out yet; I am assuming this is how they made it work more seamlessly in Windows?

There is also an input event sent to the standard keyboard which is generated when battery saver is enabled and charging reaches 85%; I have also mapped this in so that notifications can be displayed (see below in the keyboard remapping section).

### Start on lid open

To turn on or off the "Start on lid open" setting (the laptop will  power on automatically when opening the lid), there is a new device attribute created at `/sys/devices/platform/samsung-galaxybook/start_on_lid_open` which can be read from or written to. A value of 0 means "off" while a value of 1 means "on".

```sh
# read current value (0 for disabled, 1 for enabled)
cat /sys/devices/platform/samsung-galaxybook/start_on_lid_open

# turn on (supports values such as: 1, on, true, yes, etc)
echo true | sudo tee /sys/devices/platform/samsung-galaxybook/start_on_lid_open

# turn off (supports values such as: 0, off, false, no, etc)
echo 0 | sudo tee /sys/devices/platform/samsung-galaxybook/start_on_lid_open
```

### USB Charging mode

To turn on or off the "USB Charging" mode (allows USB ports to provide power even when the laptop is turned off), there is a new device attribute created at `/sys/devices/platform/samsung-galaxybook/usb_charging` which can be read from or written to. A value of 0 means "off" while a value of 1 means "on".

```sh
# read current value (0 for disabled, 1 for enabled)
cat /sys/devices/platform/samsung-galaxybook/usb_charging

# turn on (supports values such as: 1, on, true, yes, etc)
echo true | sudo tee /sys/devices/platform/samsung-galaxybook/usb_charging

# turn off (supports values such as: 0, off, false, no, etc)
echo 0 | sudo tee /sys/devices/platform/samsung-galaxybook/usb_charging
```

My own observations on how this feature appears to work (which has nothing to do with this driver itself, actually):

- Only the USB-C ports are impacted by this setting, and not the USB-A ports (at least this is the case on the Galaxy Book2 Pro).
- When the setting is turned on and you plug in a mobile phone or similar to one of the USB-C ports, then the phone will begin charging from the laptop's battery.
- When the setting is turned off and you plug in a mobile phone, the laptop battery will actually start charging from the phone's battery.

### Fan speed

Samsung has implemented the ACPI method `_FST` for the fan device, but not the other optional methods in the ACPI specification which would cause the kernel to automatically add the `fan_speed_rpm` attribute. On top of this, it seems that there are some bugs in the firmware that throw an exception when you try to execute this ACPI method. This behavior is also seen in Windows (that an ACPI exception is thrown when the fan speed is attempted to be checked).

However, I believe I have succeeded in figuring out roughly the intention of how their `_FST` method is supposed to work:

1. There is a data package `FANT` ("fan table"??) which seems to be some kind of list of possible RPM speeds that the fan can run at.
2. There is a data field on the embedded controller called `FANS` ("fan speed"??) which seems to give the current "level" that the fan is operating at.

I have **assumed** that the values from `FANT` are integers which represent the actual RPM values (they seem reasonble, anyway), but can't be one hundred percent certain. It would be interesting to get confirmation from Samsung or if someone had a way to measure the actual speed of the fan!

The fan can either be completely off (0) or one of the levels represented by the speeds in `FANT`. This driver reads the values in from `FANT` instead of hard-coding the levels with the assumption that it could be different values and a different number of levels for different devices. For reference, the values I see with my Galaxy Book2 Pro are:

- 0x0 (0)
- 0xdac (3500)
- 0xee2 (3820)
- 0x1144 (4420)
- 0x127a (4730)

On top of this, in Samsung's `_FST` method it seems to be adding `0x0a` (10) to each value before trying to report them, and that level 3 and 4 should have the same value, while level 5 should be the 4th value from `FANT`. However, real-life observation suggests that level 3 and 4 are in fact different, and that level 5 seems to be significantly louder than level 4. Due to this, this driver will just "guess" that levels 3 and 4 are actually as is listed in `FANT`, and that the last level is maybe 1000 RPM faster than level 4 (unless anyone can find something better than this!).

The fan speed can be monitored using hwmon sensors or by reading the `fan_speed_rpm` sysfs attribute.

```sh
# read current fan speed rpm from sysfs attribute
cat /sys/bus/acpi/devices/PNP0C0B\:00/fan_speed_rpm

# read current fan speed rpm from hwmon device
sensors
```

### Performance mode

To modify the "performance mode", the driver implements the [`platform_profile` interface](https://www.kernel.org/doc/html/latest/userspace-api/sysfs-platform_profile.html). The following strings can be written to `/sys/firmware/acpi/platform_profile` to set the performance mode:

- `low-power` (Silent)
- `quiet` (Quiet)
- `balanced` (Optimized, default value if not set)
- `performance` (High Performance)

Examples:

```sh
# Get supported performance modes
cat /sys/firmware/acpi/platform_profile_choices

# set performance_mode to low-power
echo low-power | sudo tee /sys/firmware/acpi/platform_profile

# get current performance_mode
cat /sys/firmware/acpi/platform_profile
```

**Note:** To match the logic in the Windows driver, as well as avoid causing issues with other features, the driver currently will always set the performance mode to "Optimized" every time during its initialization (e.g. upon startup).

It should be possible to set your own desired startup performance mode or to save and restore the mode across reboots, you can eiter use a startup script or install TLP, power-profiles-daemon, or similar.

#### Limitations reading performance_mode

Unfortunately, I have still not been able to find any way to read the value of the "current" performance mode from the system.

At the same time, I have a suspicion that this might actually be controlled in Windows somehow at the software level. I did a small test where I set the value to "High performance" in Windows, booted to Linux, set the value to "Quiet" using this driver,  noted a difference in the fan volume, then booted back into Windows where it was once again sitting at "High performance". My working assumption is that the Samsung software (namely, `SamsungSystemSupportEngine.exe`) is storing what latest "value" was chosen by the user in Windows, and then upon startup, is setting this value using their driver.

I suppose that we could do similar on the Linux side (and might need to, actually), either via something like `sysfsutils` or with whatever eventually comes in the "Samsung Galaxybook Extras".

In light of this, for now I have allowed reading of `/sys/firmware/acpi/platform_profile` (via `cat`, for example), but please remember that it will only echo back the value that was last set by the driver, which under normal circumstances will the one active in the hardware.

#### Does this performance_mode actually work?

This was a bit hard to test, but I tried to test by setting each different mode and then running a quick stress test using the following:

```sh
sudo stress-ng --cpu 0 --cpu-load 100 --metrics-brief --perf -t 20
```

Note that it does seem to take at least a few seconds before the setting really seems to kick in.

In the end what I found was that I could **definitely** tell a difference in the result when using the "silent" (0) mode, because the resulting number of completed operations in the stress test was significantly lower when using "silent" mode (almost half).

Subjectively, I do feel like I experienced that the fan volume was quite a bit lower in the "quiet" mode as compared to the other two, but I did not really notice any major difference in the number of completed operations from the stress test. Optimized and High Performance seemed almost the same to me. I did also notice that there might be some throttling happening when the cores reach near 100C, so maybe that is part of the problem why I could not tell a difference (not sure what is safe to adjust). This could also just be a flawed test mechanism, as well!

## Galaxybook Extras

The "extras" I have added here will be intended to mimic some of the functionality that the windows Samsung Settings application and service provide for this device.

Currently I have just created a few simple shell scripts which provide the following functionality:

- Toggle keyboard brightness level
- Toggle camera on/off
- Add hwdb mapping for some unrecognized keyboard events
- Display notifications on the screen when certain keys are pressed or events occur
- Add GNOME Custom Keyboard Shortcuts to link these key mappings to the various scripts

Once you have built, installed, and loaded the platform driver then you can also make use of these "extras". The various scripts and configuration files can be installed using the [install-extras.sh](./install-extras.sh) script.

It could be possible to build some sort of service or more robust solution going forward, but this "quick and dirty" sort of works for now!

For notifications based on hotkey presses, I have just relied on `notify-send` which is also not ideal, but for a quick solution it does work for now.

## Keyboard scancode remapping

The provided file [61-keyboard-samsung-galaxybook.hwdb](./resources/61-keyboard-samsung-galaxybook.hwdb) will correct some keyboard mappings as follows:

- The "Settings" key (Fn+F1) is mapped to `config` which seem to automatically launch `gnome-control-center` and I assume might work for other desktop environments? Otherwise a shortcut can be created in your own environment to this key
- The "Touchpad toggle" key (Fn+F5) is mapped to `F21` as this is typically recognized in Linux as the touchpad toggle key (tested as working in GNOME 45.x)
- The "Keyboard backlight" key (Fn+F9) is a multi-level toggle key which does not work in the same way as the standard on/off toggle or up+down keys which are typically available. So instead, this has been mapped to `prog2` so that it can be mapped as a custom keyboard shortcut and then some additional software (e.g. a script or service) can handle rotating through the different brightness levels.
- The "Camera privacy toggle" key (Fn+F10) is mapped to `camera` so it can also be mapped to a custom keyboard shortcut.
- The "Fn Lock toggle" key (Fn+F12) generates two different events: one when it is turned "on", and a different one when it is turned "off". I have mapped them to F14 (XF86Launch5) and F15 (XF86Launch6) respectively, just so that these can also be mapped to a custom keyboard shortcut for the sake of some kind of on-screen notification
- If you have enabled `battery_saver`, when the batter charge reaches 85% and stops then an input event is automatically generated to the standard keyboard device. This event I have mapped to `prog4` so that it too can be mapped to a custom keyboard shortcut for the sake of some kind of on-screen notification
- Unforunately, the "Performance mode" key (Fn+F11) does not seem to report from the standard keyboard device, and I have not yet found where its input can be monitored, so it is still not currently supported.

### Matching additional device keyboards

Currently, these keyboard mapping rules should apply to all Galaxy Book2 and Book3 series notebooks by matching on an "svn" starting with "Samsung" (case insensitve) plus a "pn" string of three digits followed by any one of the following suffixes:

- `XED` (Galaxy Book2 series)
- `QED` (Galaxy Book2 360 series)
- `XFG` (Galaxy Book3 series)
- `QFG` (Galaxy Book3 360 series)
- `XFH` (Galaxy Book3 Ultra series)

This is quite a broad filter string but my hope is that the keyboard mappings should actually work for all of these models as from what I have seen, they all seem to have a similar keyboard layout.

In case you have issues where the mapping does not seem to be picked up on your device, then we might need to modify the filter string. You can get your own device's evdev dmi string like this:

```sh
sudo evemu-describe
```

Select the device for your "regular" keyboard (e.g. "/dev/input/event2: AT Translated Set 2 keyboard") and then you should see the full DMI string for your keyboard, like this:

```sh
# EVEMU 1.3
# Kernel: 6.5.0-13-generic
# DMI: dmi:bvnAmericanMegatrendsInternational,LLC.:bvrP11RGF.057.230404.ZQ:bd04/04/2023:br5.25:svnSAMSUNGELECTRONICSCO.,LTD.:pn950XED:pvrP11RGF:rvnSAMSUNGELECTRONICSCO.,LTD.:rnNP950XED-KA2SE:rvrSGLB208A0U-C01-G001-S0001+10.0.22000:cvnSAMSUNGELECTRONICSCO.,LTD.:ct10:cvrN/A:skuSCAI-ICPS-A5A5-ADLP-PRGF:
# Input device name: "AT Translated Set 2 keyboard"
# Input device ID: bus 0x11 vendor 0x01 product 0x01 version 0xab41
# ...
# Properties:
N: AT Translated Set 2 keyboard
I: 0011 0001 0001 ab41
```

The filter string in the hwdb file will need to modified so that it also matches your device. If you wish then you can create an issue here with the output of your `evemu-describe` and I can try to modify the file for you.

Otherwise, feel free to test yourself by modifying `/etc/udev/hwdb.d/61-keyboard-samsung-galaxybook.hwdb` and either restart or reload udev as follows:

```sh
sudo systemd-hwdb update
sudo udevadm trigger
```

Once we can put together a more definitive filter that supports more devices then I think it would make sense to ask for this keyboard mapping to get moved to systemd upstream. See: <https://github.com/systemd/systemd/blob/main/hwdb.d/60-keyboard.hwdb>
