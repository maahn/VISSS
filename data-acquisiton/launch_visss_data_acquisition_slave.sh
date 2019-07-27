#!/usr/bin/env bash
df -H | grep /data | awk '{ print "STORAGE USED " $5 " " $6 }'

PATH=/home/visss/Desktop/VISSS/
EXE=$PATH/data-acquisiton/visss-data-acquisiton
OUTDIR=/data/test
/bin/mkdir -p $OUTDIR/logs

set -o pipefail

cd $PATH/data-acquisiton/
MTU=$(/bin/cat /sys/class/net/eno1/mtu)
if [[ "$MTU" != "8960" ]]
then
	/usr/bin/sudo /home/visss/DALSA/GigeV/bin/gev_nettweak eno1
	/bin/echo "It takes some time for the camera to come online... Sleep 25"
	/bin/sleep 25
fi

/usr/bin/sudo /sbin/setcap cap_sys_nice+ep $EXE

/bin/echo "It takes some time for the camera to come online... Sleep 25"
/bin/sleep 25


for (( ; ; ))
do

timestamp=$(/bin/date +%FT%T)

if $EXE -o=/data/test $PATH/camera-configuration/visss_slave.config  | /usr/bin/tee $OUTDIR/logs/$HOSTNAME_$timestamp.txt
		then
			/bin/echo "worked"
			exit
else
	/bin/echo "Didn't work, trying again in 5s"
	/usr/bin/paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	/bin/sleep 5
fi
done
