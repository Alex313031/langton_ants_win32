#!/bin/bash

export HERE=${PWD} &&
export TARGET=../src/res &&

ffmpeg -y -i ${HERE}/chomp.mp3 -filter:a "volume=0.25" -c:a adpcm_ms -ar 48000 -ac 2 ${TARGET}/ants.wav &&

exit 0
