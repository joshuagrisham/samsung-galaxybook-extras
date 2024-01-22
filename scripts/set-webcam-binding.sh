#!/bin/sh

set -e

{
    echo $1 | sudo tee /sys/bus/usb/drivers/usb/$2
} 1>/dev/null
