#!/usr/bin/env bash
df -H | grep /data | awk '{ print "STORAGE USED " $5 " " $6 }'

ROOTPATH=/home/visss/Desktop/VISSS/
EXE=$ROOTPATH/data-acquisiton/visss-data-acquisiton
OUTDIR=/data/lim
HOST=`hostname`
/bin/mkdir -p $OUTDIR/logs

set -o pipefail

cd $ROOTPATH/data-acquisiton/

MTU=$(/bin/cat /sys/class/net/enp35s0f0/mtu)
if [[ "$MTU" != "9216" ]]
then
	/usr/bin/sudo /home/visss/DALSA/GigeV/bin/gev_nettweak enp35s0f0
	/bin/echo "It takes some time for the camera to come online... Sleep 25"
	/bin/sleep 25
fi

/bin/echo "Set camera IP address (just to be sure)"
/usr/local/bin/gevipconfig -p 00:01:0D:C3:0F:34 192.168.100.2 255.255.255.0


/usr/bin/sudo /sbin/setcap cap_sys_nice+ep $EXE


for (( ; ; ))
do

timestamp=$(/bin/date +%FT%T)
#if $EXE -p=veryfast -q=17 -o=$OUTDIR $ROOTPATH/camera-configuration/visss_master.config | /usr/bin/tee $OUTDIR/logs/$HOST-$timestamp.txt
if $EXE -p=fast -q=17 -o=$OUTDIR $ROOTPATH/camera-configuration/visss_master.config | /usr/bin/tee $OUTDIR/logs/$HOST-$timestamp.txt
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