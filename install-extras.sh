#!/bin/sh

set -e

echo "Samsung Galaxybook systemd keyboard hwdb and GNOME Fn Lock notification installation"

IS_MODULE_LOADED=$(lsmod | grep samsung_galaxybook &2>/dev/null)
if [ -z "$IS_MODULE_LOADED" ]; then
    echo "Required kernel module 'samsung_galaxybook' is missing; please ensure that it is built, installed, and loaded first!"
    echo "See driver/README.md for more information."
    exit 1
else
    echo "Found required module 'samsung_galaxybook'; continuing..."
    echo
fi

echo "Copying scripts..."
sudo rm -rf /opt/samsung-galaxybook-extras
sudo mkdir -p /opt/samsung-galaxybook-extras
sudo cp scripts/* /opt/samsung-galaxybook-extras/

echo "Adding systemd hwdb configuration for keyboard keys..."
sudo cp resources/61-keyboard-samsung-galaxybook.hwdb /etc/udev/hwdb.d/
sudo systemd-hwdb update
sudo udevadm trigger

echo
printf 'Reset existing GNOME Custom Keyboard Shortcuts and replace with Fn Lock notification shortcuts? (y/n) '
read SHOULD_IMPORT_KEYBINDINGS

if [ "$SHOULD_IMPORT_KEYBINDINGS" != "${SHOULD_IMPORT_KEYBINDINGS#[Yy]}" ]; then

    # For possible key names, see: https://github.com/GNOME/gtk/blob/main/gdk/keynames.txt
    # These need to be matched to the values in 61-keyboard-samsung-galaxybook.hwdb

    kb=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings
    dconf reset -f $kb/

    dconf write $kb/custom0/name "'Fn Lock On Notification'"
    dconf write $kb/custom0/binding "'Launch5'"
    dconf write $kb/custom0/command "'/opt/samsung-galaxybook-extras/notify-fn-lock-on.sh'"

    dconf write $kb/custom1/name "'Fn Lock Off Notification'"
    dconf write $kb/custom1/binding "'Launch6'"
    dconf write $kb/custom1/command "'/opt/samsung-galaxybook-extras/notify-fn-lock-off.sh'"

    dconf write $kb "['$kb/custom0/', '$kb/custom1/']"

    echo "Completed reset and import of GNOME Custom Keyboard Shortcuts."

else
    echo "GNOME Custom Keyboard Shortcuts import was skipped."
fi

echo
echo "Samsung Galaxybook systemd keyboard hwdb / GNOME Fn Lock notification installation is now complete."
