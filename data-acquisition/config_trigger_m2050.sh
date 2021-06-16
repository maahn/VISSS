#!/usr/bin/env bash

IP='192.168.100.2'
MAC='00:01:0D:C5:59:C6'
INTERFACE='enp3s0'
MAXMTU='9216'
SITE='TEST'

PRESET='ultrafast'
QUALITY=17

CAMERACONFIG='visss_trigger.config'
TRIGGERFOLLOWER='trigger'

ROOTPATH=/home/visss/Desktop/VISSS/
OUTDIR=/home/visss/testdata
HOST=`hostname`
EXE=$ROOTPATH/data-acquisition/visss-data-acquisition
