#!/usr/bin/env bash

echo "<html><pre>"
hostname
date

echo "********************************************************************************"
echo uptime
uptime

echo "********************************************************************************"
echo ps -eo pcpu,pmem,pid,user,args | sort -r -k1 | head
ps -eo pcpu,pmem,pid,user,args | sort -r -k1 | head

echo "********************************************************************************"
echo df -h / /data
df -h / /data

echo "</pre></html>"