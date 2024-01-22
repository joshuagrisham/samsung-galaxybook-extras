#!/bin/sh

# The potential "right way" in GNOME is to modify the GNOME Camera Privacy Settings
# via the key `org.gnome.desktop.privacy.disable-camera` but this only affects Flatpak applications,
# so to get an actual "working" solution we will use driver binding/unbinding
# See here for more information: https://gitlab.gnome.org/GNOME/gnome-control-center/-/issues/741

# Thus this script will "disable" a webcam by unbinding and rebinding its driver
# In reality this script would work for any USB device, it is just that we are
# targeting a specific Vendor ID + Product ID for the webcam in the Samsung Galaxy Book2 Pro
# For more information on driver binding and unbinding, see: https://lwn.net/Articles/143397/

set -e

WEBCAM_VENDOR_ID='2b7e'
WEBCAM_PRODUCT_ID='c556'

VENDOR_MATCH=$(grep --with-filename $WEBCAM_VENDOR_ID /sys/bus/usb/devices/*/idVendor)
PRODUCT_MATCH=$(grep --with-filename $WEBCAM_PRODUCT_ID "`dirname $VENDOR_MATCH`/idProduct")

WEBCAM_USB_DEVICE_ID=$(basename $(dirname $PRODUCT_MATCH))

# Unbind if driver is bound; otherwise bind if driver is not already bound
if [ -e "/sys/bus/usb/devices/$WEBCAM_USB_DEVICE_ID/driver" ]; then
    sudo /opt/samsung-galaxybook-extras/set-webcam-binding.sh $WEBCAM_USB_DEVICE_ID unbind
    # If notify-send is installed then pop up a notification
    if command -v notify-send > /dev/null 2>&1; then
        notify-send --transient \
            --icon=camera-disabled \
            --category=device \
            "Camera Disabled" "The camera is now disabled."
    fi
else
    sudo /opt/samsung-galaxybook-extras/set-webcam-binding.sh $WEBCAM_USB_DEVICE_ID bind
    # If notify-send is installed then pop up a notification
    if command -v notify-send > /dev/null 2>&1; then
        notify-send --transient \
            --icon=camera \
            --category=device \
            "Camera Enabled" "The camera is now enabled."
    fi
fi
