#!/usr/bin/env bash
EXE=/home/visss/Desktop/VISSS/data-acquisiton/visss-data-acquisiton

for (( ; ; ))
do
if $EXE -f=10 -q=15 -l=1 -p=medium -o=/data/test /home/visss/Desktop/VISSS/camera-configuration/visss_calibration.config
		then
			echo "worked"
			exit
else
	echo "Didn't work, trying again in 5s"
	paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	sleep 5
fi
done
