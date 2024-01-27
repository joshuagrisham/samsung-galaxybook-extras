#!/bin/sh

set -e

# If notify-send is installed then pop up a notification
if command -v notify-send > /dev/null 2>&1; then
    notify-send --transient \
        --icon=lock \
        --category=device \
        "Fn Lock Enabled" "The Fn lock has been enabled."
fi
