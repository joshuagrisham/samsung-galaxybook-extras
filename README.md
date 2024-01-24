# Samsung Galaxybook Extras

Samsung Galaxybook Linux platform driver and accompanying utilities.

The intent is to somewhat replicate in Linux what Samsung has done in Windows with what I think the following components are doing:

- **Samsung System Event Controller**: an ACPI driver that interacts with their `SCAI` ACPI device in order to control a lot of these settings.
- **Samsung System Platform Engine**: a service which runs in the background to handle a few things including (I think) things like automatically turning off the keyboard backlight after a configured idle timer, etc.
- **Samsung Settings**: GUI application to configure all of the different available features.

## Platform Driver

This is a new and (currently) completely out-of-tree kernel platform driver intended to mimic what the **Samsung System Event Controller** seems to be doing in Windows (namely, communicate with the `SCAI` ACPI device in order to control these extra features). Once more features have been added and it has been tested then my intention was to try and submit the driver to be added to the kernel.

`cd` over to the [driver/](./driver/) folder and follow the README there for compiling and installing the module.

The following features are currently implemented:

- Keyboard backlight
- USB ports provide charging when device is turned off
- Performance mode (High performance, Optimized, Quiet, Silent) *note: setting is supported and seems to be working, but I have not found a way to read the current value from the device; see below*

I have found the following features in my other traces and suspect it might be possible to implement endpoints to control these as well (requires additional debugging and development):

- "Dolby Atmos" mode for the speakers
- Battery saver (stop charging at 85%)
- Start device automatically when opening lid

### Keyboard backlight

A new LED class called `samsung-galaxybook::kbd_backlight` is created which can be controlled via `sysfs` at `/sys/class/leds/samsung-galaxybook::kbd_backlight/brightness` (values 0 to 3) or by many of the standard utilities such as `brightnessctl`, `light`, etc.

It also seems to be picked up automatically in GNOME 45.x in the panel, where you can click the arrow beside `Keyboard` and adjust the slider:

![GNOME Panel Keyboard Backlight](./resources/keyboard-backlight-gnome.png "GNOME Panel Keyboard Backlight")

I have also included a simple `toggle-keyboard-brightness` script along with the "extras" package as well as remapped the keyboard key Fn+F9 so that it will execute this script (assuming you are using GNOME).

Note that the setting "automatically turn off the keyboard backlight after X seconds" in Windows is actually controlled by Samsung's application service and not by the device driver itself; if such a feature is desired then it would need to be a similar software-based solution (e.g. added to the "extras" or something).

### USB Charging mode

To turn on or off the "USB Charging" mode (allows USB ports to provide power even when the laptop is turned off), there is a new device attribute created at `/sys/bus/platform/devices/samsung-galaxybook/usb_charging` which can be read from or written to. A value of 0 means "off" while a value of 1 means "on".

```sh
# read current value (0 for disabled, 1 for enabled)
cat /sys/bus/platform/devices/samsung-galaxybook/usb_charging

# turn on (supports values such as: 1, on, true, yes, etc)
echo true | sudo tee /sys/bus/platform/devices/samsung-galaxybook/usb_charging

# turn off (supports values such as: 0, off, false, no, etc)
echo 0 | sudo tee /sys/bus/platform/devices/samsung-galaxybook/usb_charging
```

### Performance mode

To modify the "performance mode", there is a new device attribute created at `/sys/bus/platform/devices/samsung-galaxybook/performance_mode` which can be written to. You can write to this value with either a number (0 through 3) or with one of the modes' text-based names:

- Silent
- Quiet
- Optimized
- High performance

Using these text-based names is not case sensitive so you can write in upper, lower, or mixed case.

Examples:

```sh
# set performance_mode to Silent
echo 0 | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode
echo silent | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode

# set performance_mode to Quiet
echo 1 | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode
echo QUIET | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode

# set performance_mode to Optimized
echo 2 | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode
echo OptiMIzed | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode

# set performance_mode to High performance
echo 3 | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode
echo High Performance | sudo tee /sys/bus/platform/devices/samsung-galaxybook/performance_mode
```

#### Limitations reading performance_mode

Unfortunately, I have still not been able to find any way to read the value of the "current" performance mode from the system.

At the same time, I have a suspicion that this might actually be controlled in Windows somehow at the software level. I did a small test where I set the value to "High performance" in Windows, booted to Linux, set the value to "Quiet" using this driver,  noted a difference in the fan volume, then booted back into Windows where it was once again sitting at "High performance". My assumption is that the Samsung software (e.g. the Samsung System Platform Engine service among others) is storing what latest "value" was chosen by the user in Windows, and then upon startup, is setting this value using their driver.

I suppose that we could do similar on the Linux side (and might need to, actually), either via something like `sysfsutils` or with whatever eventually comes in the "Samsung Galaxybook Extras".

In light of this, for now I have allowed reading of `/sys/bus/platform/devices/samsung-galaxybook/performance_mode` (via `cat`, for example), but please remember that it will only give the correct value if a value has first been set by the driver since the last time the driver has been loaded (a restart, for example). The "latest" set value is just stored in a local variable in the driver code itself, and every time the module is loaded, it will start with displaying a default value of 0 no matter what the real "perfomance mode" is at that time. I have considered changing the attribute to read-only, but for now if you do read the value, then there will be a warning printed in the kernel log which attempts to highlight this problem/limitation. Here is an example of the warning:

> [ 3020.445850] samsung_galaxybook: TODO: performance_mode was not read from the device, but instead only displays the latest value which has been set from the driver. It is suspected that persisting and restoring this value across restarts will need to be implemented in the userspace.

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
- Correct some of the keyboard mappings
- Add GNOME Custom Keyboard Shortcuts to 

Once you have built, installed, and loaded the platform driver then you can also make use of these "extras". The various scripts and configuration files can be installed using the [install-extras.sh](./install-extras.sh) script.

It could be possible to build some sort of service or more robust solution going forward, but this "quick and dirty" sort of works for now!

For notifications based on hotkey presses, I have just relied on `notify-send` which is also not ideal, but for a quick solution it does work for now.

## Keyboard scancode remapping

The provided file [61-keyboard-samsung-galaxybook.hwdb](./resources/61-keyboard-samsung-galaxybook.hwdb) will correct some keyboard mappings as follows:

- The "Settings" key (Fn+F1) is mapped to `prog1` so it can later be mapped as a custom keyboard shortcut
- The "Touchpad toggle" key (Fn+F5) is mapped to `F21` as this is typically recognized in Linux as the touchpad toggle key (tested as working in GNOME 45.x)
- The "Keyboard backlight" key (Fn+F9) is a multie-level toggle key which does not work in the same way as the standard on/off toggle or up+down keys which are available. So instead, this has been mapped to `prog2` so that it can be mapped as a custom keyboard shortcut and then some additional software (e.g. a script or service) can handle the different levels.
- The "Camera privacy toggle" key (Fn+F10) is mapped to `prog3` so it can also be mapped to a custom keyboard shortcut
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
