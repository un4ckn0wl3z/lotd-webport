#!/bin/sh
# Native test run: ./tools/shots.sh "<PORT_INPUT script>" "<frames to screenshot>" [seconds]
# Runs build/port.exe from build/ with a fresh save folder, then converts the screenshots to PNG.
cd "$(dirname "$0")/../build" || exit 1
[ -n "$KEEP_SAVE" ] || rm -rf save; rm -f shot_*.bmp shot_*.png
PORT_INPUT="$1" PORT_SHOT="$2" timeout "${3:-120}" ./port.exe > run.out 2>&1
grep -A14 "FATAL" port.log | head -24
python -c "
import glob
from PIL import Image
for f in sorted(glob.glob('shot_*.bmp')):
    Image.open(f).save(f[:-4] + '.png'); print(f[:-4] + '.png')
"
