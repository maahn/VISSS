#!/usr/bin/env bash

echo "<html><pre>"
hostname
date

echo "********************************************************************************"
echo uptime
uptime

echo "********************************************************************************"
echo "ps  --sort=-pcpu -eo pcpu,pmem,pid,user,args | head"
ps  --sort=-pcpu -eo pcpu,pmem,pid,user,args | head

echo "********************************************************************************"
echo df -h / /data
df -h / /data

echo "********************************************************************************"
echo chronyc sources -v
chronyc sources -v

echo "********************************************************************************"
echo systemctl status phc2sys.service
systemctl status phc2sys.service

echo "********************************************************************************"
echo systemctl status ptp4l.service
systemctl status ptp4l.service



echo "</pre></html>"