#!/usr/bin/env bash
PATH=/home/visss/Desktop/VISSS/
EXE=$PATH/data-acquisiton/visss-data-acquisiton
cd $PATH/data-acquisiton/
sudo /home/visss/DALSA/GigeV/bin/gev_nettweak eno1
sudo /sbin/setcap cap_sys_nice+ep $EXE


echo "It takes some time for the camera to come online... Sleep 15"
sleep 25


for (( ; ; ))
do
if $EXE -p=superfast -o=/data/test $PATH/camera-configuration/visss_slave.config
		then
			echo "worked"
			exit
else
	echo "Didn't work, trying again in 5s"
	paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	sleep 5
fi
done
