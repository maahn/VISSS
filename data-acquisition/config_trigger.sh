#!/usr/bin/env bash

IP='192.168.100.2'
MAC='00:01:0D:C3:0F:34'
INTERFACE='enp35s0f0'
MAXMTU='9216'
SITE='LIM'

PRESET='ultrafast'
QUALITY=17

CAMERACONFIG='visss_trigger.config'
TRIGGERFOLLOWER='trigger'

ROOTPATH=/home/visss/Desktop/VISSS/
OUTDIR=/data/lim
HOST=`hostname`
EXE=$ROOTPATH/data-acquisition/visss-data-acquisition