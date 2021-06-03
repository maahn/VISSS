#!/usr/bin/env bash

#load settings
source $"$(dirname "$0")/$1"

if [ -z "$IP" ]
	then echo "variable IP not set. EXIT"
	exit
fi


df -H | grep /data | awk '{ print "STORAGE USED " $5 " " $6 }'

/bin/mkdir -p $OUTDIR/logs

set -o pipefail

cd $ROOTPATH/data-acquisition/

MTU=$(/bin/cat /sys/class/net/$INTERFACE/mtu)
if [[ "$MTU" != "$MAXMTU" ]]
then
	/usr/bin/sudo /home/visss/DALSA/GigeV/bin/gev_nettweak $INTERFACE
	/bin/echo "It takes some time for the camera to come online... Sleep 25"
	/bin/sleep 25
fi

if ping -c 1 $IP > /dev/null
	then
	echo 'camera repsonding'
else
	/bin/echo "Set camera IP address (just to be sure)"
	/usr/local/bin/gevipconfig -p $MAC $IP 255.255.255.0
fi

/usr/bin/sudo /sbin/setcap cap_sys_nice+ep $EXE


for (( ; ; ))
do

timestamp=$(/bin/date +%FT%T)
COMMAND="$EXE -p=$PRESET -q=$QUALITY -o=$OUTDIR -s=$SITE $ROOTPATH/camera-configuration/$CAMERACONFIG $IP| /usr/bin/tee $OUTDIR/logs/$TRIGGERFOLLOWER-$timestamp.txt"
echo $COMMAND
if $COMMAND
		then
			/bin/echo "worked"
			exit
else
	/bin/echo "Didn't work, trying again in 5s"
	/usr/bin/paplay /usr/share/sounds/ubuntu/stereo/dialog-question.ogg
	/bin/sleep 5
fi
done


#new config

#superfast
#18: 91 4300
#23: 12

#veryfast
#18: 0.4
#23 0.09


#old
#superfast
#23: 0.41
#20:13
#21: 2!!
#18: 90

#veryfast
#18: 0.187
#23: 0.06


#190 MB per 5min
#38 MB per min
# 4mb per 1000 images!
