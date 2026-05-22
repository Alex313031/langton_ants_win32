#!/bin/bash

export HERE=${PWD} &&
export TARGET=../src/res &&

ffmpeg -y -i ${HERE}/chomp.mp3 -filter:a "volume=0.25" -c:a adpcm_ms -ar 48000 -ac 2 ${HERE}/temp.wav &&

ffmpeg -stream_loop 7 -i ${HERE}/temp.wav -c copy ${TARGET}/ants.wav &&

ffmpeg -y -i ${HERE}/ding.wav -filter:a "volume=1.00" -c:a adpcm_ms -ar 44100 -ac 2 ${TARGET}/notify.wav &&

rm -fv ${HERE}/temp.wav &&

exit 0
