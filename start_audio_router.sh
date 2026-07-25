#!/bin/bash

AUDIO_ROUTER="/Users/jcarrell/Documents/repos/audio_router/audio_router"

if "$AUDIO_ROUTER" -b 128 -i GX -o 2i2; then
    exit 0
fi

if "$AUDIO_ROUTER" -b 128 -i GX -o "MacBook Pro Speakers" -b 128; then
    exit 0
fi

osascript -e 'display notification "Could not start audio router. No valid input/output device found." with title "Audio Router" sound name "Glass"'
