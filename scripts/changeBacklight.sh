#!/bin/bash

BACKLIGHT_PATH="/sys/class/backlight/intel_backlight/brightness"
MAX_BRIGHTNESS_PATH="/sys/class/backlight/intel_backlight/max_brightness"
CURRENT_BRIGHTNESS=$(cat "$BACKLIGHT_PATH")
MAX_BRIGHTNESS=$(cat "$MAX_BRIGHTNESS_PATH")
STEP=40  # Adjust the step size for how much the brightness changes

echo "Current Brightness: $CURRENT_BRIGHTNESS"
echo "Maximum Brightness: $MAX_BRIGHTNESS"


if [[ $1 == "up" ]]; then
    NEW_BRIGHTNESS=$((CURRENT_BRIGHTNESS + STEP))
    if [[ $NEW_BRIGHTNESS -gt $MAX_BRIGHTNESS ]]; then
        NEW_BRIGHTNESS=$MAX_BRIGHTNESS
    fi
elif [[ $1 == "down" ]]; then
    NEW_BRIGHTNESS=$((CURRENT_BRIGHTNESS - STEP))
    if [[ $NEW_BRIGHTNESS -lt 0 ]]; then
        NEW_BRIGHTNESS=0
    fi
fi

echo $NEW_BRIGHTNESS | sudo tee "$BACKLIGHT_PATH"
