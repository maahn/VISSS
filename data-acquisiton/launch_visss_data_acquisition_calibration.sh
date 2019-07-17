#!/usr/bin/env bash
PATH=/home/visss/Desktop/VISSS/
EXE=$PATH/data-acquisiton/visss-data-acquisiton
cd $PATH/data-acquisiton/


for (( ; ; ))
do
if $EXE -f=10 -q=15 -l=1 -p=medium -o=/data/test $PATH/camera-configuration/visss_calibration.config
		then
			echo "worked"
			exit
else
	echo "Didn't work, trying again in 5s"
	paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	sleep 5
fi
done
