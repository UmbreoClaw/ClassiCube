#!/bin/sh
# usage: capture.sh <mode> <name> [width] [height] [seconds]
S=${RT_TEST_DIR:-$HOME/rt-test}
MODE=$1; NAME=$2; W=${3:-640}; H=${4:-480}; SECS=${5:-30}
cd $S/run
printf "gfx-raytracing=$MODE\nwindow-width=$W\nwindow-height=$H\nviewdist=64\nfpslimit=30FPS\nhacks-hacksenabled=true\n$EXTRA" > options.txt
rm -f client.log
(DISPLAY=:99 nohup timeout $((SECS+15)) ./${BIN:-ClassiCube} $S/run/maps/scene.cw > $S/game_$NAME.log 2>&1 &)
python3 -c "import time; time.sleep($SECS)"
cp $S/fb/Xvfb_screen0 $S/$NAME.xwd && python3 $S/xwd2png.py $S/$NAME.xwd $S/$NAME.png
pkill -f "${BIN:-ClassiCube} $S/run/maps/scene.cw"
python3 -c "import time; time.sleep(1)"
grep -v "^  Connecting\|^HTTP\|^Download\|Fetching\|Adding http" $S/game_$NAME.log | tail -4
