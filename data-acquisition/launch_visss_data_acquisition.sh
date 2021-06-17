#!/usr/bin/env bash



#echo $@

set -u
SETTINGS=""

#parse arguments 
#only overides settings file if present
while [ $# -gt 0 ]; do
  case "$1" in
    --IP=*)
      IP="${1#*=}"
      ;;
    --MAC=*)
      MAC="${1#*=}"
      ;;
    --INTERFACE=*)
      INTERFACE="${1#*=}"
      ;;
    --MAXMTU=*)
      MAXMTU="${1#*=}"
      ;;
    --PRESET=*)
      PRESET="${1#*=}"
      ;;
    --QUALITY=*)
      QUALITY="${1#*=}"
      ;;
    --CAMERACONFIG=*)
      CAMERACONFIG="${1#*=}"
      ;;
    --NAME=*)
      NAME="${1#*=}"
      ;;
    --ROOTPATH=*)
      ROOTPATH="${1#*=}"
      ;;
    --OUTDIR=*)
      OUTDIR="${1#*=}"
      ;;
    --SITE=*)
      SITE="${1#*=}"
      ;;
    --SETTINGS=*)
      SETTINGS="${1#*=}"
      ;;
    *)
      printf "***************************\n"
      printf "* Error: Invalid argument.*\n"
      printf "$1 ${1#*=}\n"
      printf "***************************\n"
      exit 1
  esac
  shift
done

# read old settings  file
if [ ! -z "$SETTINGS" ]; then   
  SETTINGS=$"$(dirname "$0")/$SETTINGS"
  if [ -f $SETTINGS ]; then
    echo "Reading SETTINGS"
    source "$SETTINGS"
  else
    echo "SETTINGS $SETTINGS does not exist."
    exit 1
  fi
fi


HOST=`hostname`
EXE=$ROOTPATH/visss-data-acquisition

if [ -z "$NAME" ]; then   
  NAME="${CAMERACONFIG%.*}"
fi
if [ -z "$IP" ]
	then echo "variable IP not set. EXIT"
	exit
fi


df -H | grep /data | awk '{ print "STORAGE USED " $5 " " $6 }'

/bin/mkdir -p $OUTDIR/logs

set -o pipefail

cd $ROOTPATH

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
	COMMAND="$EXE -p=$PRESET -q=$QUALITY -o=$OUTDIR -n=$NAME -s=$SITE $CAMERACONFIG $IP| /usr/bin/tee $OUTDIR/logs/$NAME-$timestamp.txt"
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
