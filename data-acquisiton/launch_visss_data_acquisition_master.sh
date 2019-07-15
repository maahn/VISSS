#!/usr/bin/env bash

EXE=/home/visss/Desktop/VISSS/data-acquisiton/visss-data-acquisiton
sudo /home/visss/DALSA/GigeV/bin/gev_nettweak eno1
sudo /sbin/setcap cap_sys_nice+ep $EXE

echo "It takes some time for the camera to come online... Sleep 15"
sleep 15

for (( ; ; ))
do

if $EXE -p=superfast -o=/data/test /home/visss/Desktop/VISSS/camera-configuration/visss_master.config
		then
			echo "worked"
			exit
else
	echo "Didn't work, trying again in 5s"
	sleep 5
fi
done
