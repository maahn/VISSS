#!/usr/bin/env bash
PATH=/home/visss/Desktop/VISSS/
EXE=$PATH/data-acquisiton/visss-data-acquisiton
cd $PATH/data-acquisiton/
/usr/bin/sudo /home/visss/DALSA/GigeV/bin/gev_nettweak eno1
/usr/bin/sudo /sbin/setcap cap_sys_nice+ep $EXE


/bin/echo "It takes some time for the camera to come online... Sleep 15"
/bin/sleep 25


for (( ; ; ))
do
if $EXE -p=superfast -l=20 -o=/data/test $PATH/camera-configuration/visss_slave.config
		then
			/bin/echo "worked"
			exit
else
	/bin/echo "Didn't work, trying again in 5s"
	/usr/bin/paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	/bin/sleep 5
fi
done
