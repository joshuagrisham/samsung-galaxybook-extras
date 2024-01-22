#!/bin/sh

set -e

MAX_BRIGHTNESS=$(cat /sys/class/leds/samsung-galaxybook::kbd_backlight/max_brightness)
CURRENT_BRIGHTNESS=$(cat /sys/class/leds/samsung-galaxybook::kbd_backlight/brightness)
NEW_BRIGHTNESS=$(($CURRENT_BRIGHTNESS + 1))

if [ "$NEW_BRIGHTNESS" -gt "$MAX_BRIGHTNESS" ]; then
    NEW_BRIGHTNESS=0
fi

echo Current brightness is $CURRENT_BRIGHTNESS, will set to $NEW_BRIGHTNESS

sudo /opt/samsung-galaxybook-extras/set-keyboard-brightness.sh $NEW_BRIGHTNESS
#{
#    echo $NEW_BRIGHTNESS | sudo tee /sys/class/leds/samsung-galaxybook::kbd_backlight/brightness
#} 1>/dev/null

# If notify-send is installed then pop up a notification
if command -v notify-send > /dev/null 2>&1; then
    notify-send --transient \
        --icon=keyboard-brightness \
        --category=device \
        "Keyboard Brightness" "Brightness Level: $NEW_BRIGHTNESS/$MAX_BRIGHTNESS"
fi
