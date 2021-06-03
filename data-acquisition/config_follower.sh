#!/usr/bin/env bash

IP='192.168.200.2'
MAC='00:01:0D:C3:04:9F'
INTERFACE='enp35s0f1'
MAXMTU='9216'
SITE='LIM'

PRESET='ultrafast'
QUALITY=17

CAMERACONFIG='visss_follower.config'
TRIGGERFOLLOWER='follower'

ROOTPATH=/home/visss/Desktop/VISSS/
OUTDIR=/data/lim
HOST=`hostname`
EXE=$ROOTPATH/data-acquisition/visss-data-acquisition
