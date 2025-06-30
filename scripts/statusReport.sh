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
echo systemctl status visss_sync*
systemctl status visss_sync*

echo "********************************************************************************"
echo systemctl status visss_ptp*
systemctl status visss_ptp*

echo "********************************************************************************"
echo chronyc sources
chronyc sources


echo "</pre></html>"

