#!/bin/sh

set -e

# If notify-send is installed then pop up a notification
if command -v notify-send > /dev/null 2>&1; then
    notify-send --transient \
        --icon=battery \
        --category=device \
        "Battery Saver" "Battery saver has stopped the battery charging at 85%."
fi
