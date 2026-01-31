#!/usr/bin/env bash

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
    --ENCODING=*)
      ENCODING="${1#*=}"
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
    --FPS=*)
      FPS="${1#*=}"
      ;;
    --NTHREADS=*)
      NTHREADS="${1#*=}"
      ;;
    --STOREALLFRAMES=*)
      STOREALLFRAMES="${1#*=}"
      ;;
    --NOPTP=*)
      NOPTP="${1#*=}"
      ;;
    --NEWFILEINTERVAL=*)
      NEWFILEINTERVAL="${1#*=}"
      ;;
    --FOLLOWERMODE=*)
      FOLLOWERMODE="${1#*=}"
      ;;
    --QUERYGAIN=*)
      QUERYGAIN="${1#*=}"
      ;;
    --ROTATEIMAGE=*)
      ROTATEIMAGE="${1#*=}"
      ;;
    --MINBRIGHT=*)
      MINBRIGHT="${1#*=}"
      ;;
    --LIVERATIO=*)
      LIVERATIO="${1#*=}"
      ;;
    --SETTINGS=*)
      SETTINGS="${1#*=}"
      ;;
    --CPUNIC=*)
      CPUNIC="${1#*=}"
      ;;
    --CPUSERVER=*)
      CPUSERVER="${1#*=}"
      ;;
    --CPUSTREAM=*)
      CPUSTREAM="${1#*=}"
      ;;
    --CPUSTORAGE=*)
      CPUSTORAGE="${1#*=}"
      ;;
    --CPUOTHER=*)
      CPUOTHER="${1#*=}"
      ;;
    --CPUFFMPEG=*)
      CPUFFMPEG="${1#*=}"
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
    echo "BASH Reading SETTINGS"
    source "$SETTINGS"
  else
    echo "BASH SETTINGS $SETTINGS does not exist."
    exit 1
  fi
fi


HOST=`hostname`
EXE=$ROOTPATH/visss-data-acquisition

if [ -z "$NAME" ]; then   
  NAME="${CAMERACONFIG%.*}"
fi
if [ -z "$IP" ]
	then echo "BASH variable IP not set. EXIT"
	exit
fi


df -H | grep /data | awk '{ print "STORAGE USED " $5 " " $6 }'

set -o pipefail

cd $ROOTPATH

MTU=$(/bin/cat /sys/class/net/$INTERFACE/mtu)
if [[ "$MTU" != "$MAXMTU" ]]
then
	/usr/bin/sudo /home/$USER/VISSS/scripts/gev_nettweak $INTERFACE
	/bin/echo "BASH It takes some time for the camera to come online... Sleep 25"
	/bin/sleep 25
fi

# Pin NIC IRQs if CPUNIC is set
if [ $CPUNIC -ne -1 ]; then
    echo "BASH Pinning NIC $INTERFACE IRQs to CPU $CPUNIC"
    
    # Function to convert CPU list to hex mask
    cpu_list_to_mask() {
        local cpus=("$@")
        local mask=0
        for cpu in "${cpus[@]}"; do
            mask=$((mask | (1 << cpu)))
        done
        printf "%x" $mask
    }

    # Get all IRQs for the interface
    IRQs=($(grep -w "$INTERFACE" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '))
    
    # Pin each IRQ to the specified CPU core
    for irq in "${IRQs[@]}"; do
        MASK=$(cpu_list_to_mask $CPUNIC)
        echo "BASH Pinning IRQ $irq to CPU $CPUNIC (mask $MASK)"
        echo $MASK | sudo tee /proc/irq/$irq/smp_affinity
    done
fi

if ping -c 1 $IP > /dev/null
	then
	:
else
	/bin/echo "BASH Set camera IP address (just to be sure)"
  /bin/echo "BASH /usr/local/bin/gevipconfig -p $MAC $IP 255.255.255.0"
	/usr/local/bin/gevipconfig -p $MAC $IP 255.255.255.0
fi

for (( ; ; ))
do
  /bin/sleep 1
  # Build the command incrementally
  COMMAND="$EXE -e=$ENCODING -o=$OUTDIR -f=$FPS -n=$NAME -t=$NTHREADS -l=$LIVERATIO -s=$SITE -i=$NEWFILEINTERVAL -w=$STOREALLFRAMES -p=$NOPTP -d=$FOLLOWERMODE -q=$QUERYGAIN -r=$ROTATEIMAGE -b=$MINBRIGHT --cpuserver=$CPUSERVER --cpustream=$CPUSTREAM --cpustorage=$CPUSTORAGE --cpuother=$CPUOTHER --cpuffmpeg=$CPUFFMPEG $CAMERACONFIG $IP"
  
  # Use taskset to pin the process to CPU if CPUOTHER is set
  if [ $CPUOTHER -gt -1 ]; then
    /bin/echo "BASH Pinning $EXE to $CPUOTHER"
    COMMAND="taskset -c $CPUOTHER $COMMAND"
  fi
  
  echo "BASH $COMMAND"
  if $COMMAND
    then
      echo "BASH worked"
      exit
  else
    echo "BASH Didn't work, trying again in 5s"
    sleep 5
    echo "BASH Restarting VISSS"
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
