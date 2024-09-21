#!/bin/bash

# Define paths
KB_BACKLIGHT_PATH="/sys/class/leds/samsung-galaxybook::kbd_backlight/brightness"

# Check if the backlight path exists
if [[ ! -f "$KB_BACKLIGHT_PATH" ]]; then
    echo "Error: Keyboard backlight path not found."
    exit 1
fi

# Get current brightness
CURRENT_BRIGHTNESS=$(cat "$KB_BACKLIGHT_PATH")

# Define toggle values
ON_VALUE=3
OFF_VALUE=0

# Toggle logic
if [[ "$CURRENT_BRIGHTNESS" -eq "$OFF_VALUE" ]]; then
    NEW_BRIGHTNESS=$ON_VALUE
else
    NEW_BRIGHTNESS=$OFF_VALUE
fi

# Apply the new brightness value
echo "$NEW_BRIGHTNESS" | sudo tee "$KB_BACKLIGHT_PATH" > /dev/null