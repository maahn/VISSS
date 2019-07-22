#!/usr/bin/env bash
PATH=/home/visss/Desktop/VISSS/
EXE=$PATH/data-acquisiton/visss-data-acquisiton
cd $PATH/data-acquisiton/


for (( ; ; ))
do
if $EXE -p=superfast -l=20  -o=/data/mosaic_calibration $PATH/camera-configuration/visss_slave.config
		then
			/bin/echo "worked"
			exit
else
	/bin/echo "Didn't work, trying again in 5s"
	/usr/bin/paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	/bin/sleep 5
fi
done
