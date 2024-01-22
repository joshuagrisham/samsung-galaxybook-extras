#!/bin/sh

set -e

echo "Samsung Galaxybook Extras installation"

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
sudo mkdir -p /opt/samsung-galaxybook-extras
sudo cp scripts/* /opt/samsung-galaxybook-extras/

echo "Adding sudoers configuration..."
sudo cp resources/sudoers /etc/sudoers.d/samsung-galaxybook-extras

echo "Adding hwdb configuration for keyboard keys..."
sudo cp resources/61-keyboard-samsung-galaxybook.hwdb /etc/udev/hwdb.d/
sudo systemd-hwdb update
sudo udevadm trigger

echo
printf 'Reset existing GNOME Custom Keyboard Shortcuts and replace with Samsung Galaxybook Extras shortcuts? (y/n) '
read SHOULD_IMPORT_KEYBINDINGS

if [ "$SHOULD_IMPORT_KEYBINDINGS" != "${SHOULD_IMPORT_KEYBINDINGS#[Yy]}" ]; then

    kb=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings
    dconf reset -f $kb/

    dconf write $kb/custom0/name "'Settings'"
    dconf write $kb/custom0/binding "'Launch1'"
    dconf write $kb/custom0/command "'gnome-control-center'"

    dconf write $kb/custom1/name "'Keyboard Brightness'"
    dconf write $kb/custom1/binding "'Launch2'"
    dconf write $kb/custom1/command "'/opt/samsung-galaxybook-extras/toggle-keyboard-brightness.sh'"

    dconf write $kb/custom2/name "'Camera Privacy'"
    dconf write $kb/custom2/binding "'Launch3'"
    dconf write $kb/custom2/command "'/opt/samsung-galaxybook-extras/toggle-webcam.sh'"

    dconf write $kb "['$kb/custom0/', '$kb/custom1/', '$kb/custom2/']"

    echo "Completed reset and import of GNOME Custom Keyboard Shortcuts."

else
    echo "GNOME Custom Keyboard Shortcuts import was skipped."
fi

echo
echo "Samsung Galaxybook Extras installation is now complete."
