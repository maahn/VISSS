#!/usr/bin/env bash
PATH=/home/visss/Desktop/VISSS/
EXE=$PATH/data-acquisiton/visss-data-acquisiton
OUTDIR=/data/mosaic_calibration
/bin/mkdir -p $OUTDIR/logs

set -o pipefail

cd $PATH/data-acquisiton/


for (( ; ; ))
do
if $EXE -f=10 -q=15 -l=1 -p=medium -o=$OUTDIR $PATH/camera-configuration/visss_calibration.config
		then
			/bin/echo "worked"
			exit
else
	/bin/echo "Didn't work, trying again in 5s"
	/usr/bin/paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	/bin/sleep 5
fi
done
