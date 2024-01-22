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

Load the module:

```sh
sudo modprobe samsung-galaxybook
```

Unload the module:

```sh
sudo rmmod samsung-galaxybook
```

## How to avoid 'signature and/or required key missing'

If you want to sign the driver to avoid the message `samsung_galaxybook: module verification failed: signature and/or required key missing - tainting kernel`, then you will need to sign the module following whatever process is typical for your distribution. For Debian-based distrubutions (including Ubunutu), you can install the `linux-source` package for your current kernel and used the included keys and scripts to sign the module as follows:

```sh
sudo rmmod samsung-galaxybook

/usr/src/`uname -r`/debian/scripts/sign-module sha512 /usr/src/`uname -r`/debian/certs/signing_key.pem /usr/src/`uname -r`/debian/certs/signing_key.x509 samsung-galaxybook.ko

sudo cp samsung-galaxybook.ko /lib/modules/`uname -r`/updates/samsung-galaxybook.ko

sudo modprobe samsung-galaxybook
```
