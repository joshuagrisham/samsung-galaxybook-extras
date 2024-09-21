#!/bin/bash

# Define paths
KB_BACKLIGHT_PATH="/sys/class/leds/samsung-galaxybook::kbd_backlight/brightness"
KB_MAX_BRIGHTNESS_PATH="/sys/class/leds/samsung-galaxybook::kbd_backlight/max_brightness"

# Check if paths exist
if [[ ! -f "$KB_BACKLIGHT_PATH" || ! -f "$KB_MAX_BRIGHTNESS_PATH" ]]; then
    echo "0"
    exit 1
fi

# Get current and maximum brightness
CURRENT_BRIGHTNESS=$(cat "$KB_BACKLIGHT_PATH")
MAX_BRIGHTNESS=$(cat "$KB_MAX_BRIGHTNESS_PATH")

# Calculate percentage
if [[ "$MAX_BRIGHTNESS" -eq 0 ]]; then
    echo "0"
    exit 0
fi

PERCENTAGE=$(( CURRENT_BRIGHTNESS * 100 / MAX_BRIGHTNESS ))

# Ensure percentage is within 0-100
if [[ "$PERCENTAGE" -lt 0 ]]; then
    PERCENTAGE=0
elif [[ "$PERCENTAGE" -gt 100 ]]; then
    PERCENTAGE=100
fi

echo "$PERCENTAGE"
