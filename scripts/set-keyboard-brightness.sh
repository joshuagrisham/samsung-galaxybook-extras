#!/bin/sh

set -e

{
    echo $1 | sudo tee /sys/class/leds/samsung-galaxybook::kbd_backlight/brightness
} 1>/dev/null
