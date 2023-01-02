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

echo "</pre></html>"