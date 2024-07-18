# Samsung Galaxybook Linux Platform Driver

Compile the module against the current kernel:

```sh
make -C /lib/modules/`uname -r`/build M=$PWD
```

Install this module with your current kernel modules:

```sh
sudo make -C /lib/modules/`uname -r`/build M=$PWD modules_install
sudo depmod
```

> *Note:* if you wish to enable `debug` by default then you can add `samsung_galaxybook.debug=true` to your boot parameters.

Load the module (including enabling debugging messages):

```sh
sudo modprobe samsung-galaxybook debug=true
```

Unload the module:

```sh
sudo rmmod samsung-galaxybook
```

Uninstall the module:

```sh
sudo rm /lib/modules/`uname -r`/updates/samsung-galaxybook.ko
```

## How to avoid 'signature and/or required key missing'

If you want to sign the driver to avoid the message `samsung_galaxybook: module verification failed: signature and/or required key missing - tainting kernel`, then you will need to sign the module following whatever process is typical for your distribution. For Debian-based distrubutions (including Ubunutu), you can install the `linux-source` package for your current kernel and used the included keys and scripts to sign the module as follows:

```sh
sudo rmmod samsung-galaxybook

/usr/src/`uname -r`/debian/scripts/sign-module sha512 /usr/src/`uname -r`/debian/certs/signing_key.pem /usr/src/`uname -r`/debian/certs/signing_key.x509 samsung-galaxybook.ko

sudo cp samsung-galaxybook.ko /lib/modules/`uname -r`/updates/samsung-galaxybook.ko

sudo modprobe samsung-galaxybook debug=true
```

## Enable or disable features using parameters

The module parameters can be used to enable or disable most features. For example, the following would reload the module with only the core settings flags (`battery_saver`, `start_on_lid_open`, etc) and the kbd_backlight LED class, and all other features would be disabled:

```sh
sudo rmmod samsung-galaxybook
sudo modprobe samsung-galaxybook debug=false kbd_backlight=on performance_mode=off fan_speed=off i8042_filter=off acpi_hotkeys=off wmi_hotkeys=off
```

Note that these can also be added to the boot parameters (e.g. `samsung_galaxybook.fan_speed=off`).
