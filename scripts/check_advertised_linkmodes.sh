#!/bin/bash

#Script to check and change the link modes of the ethernet connections to both cameras if necessary 

link_mode_1=$(sudo ethtool enp4s0 | grep "Advertised link modes: " | awk '{print $4}')

echo "Advertised link modes for enp4s0:"
echo "$link_mode_1"

while [[ "$link_mode_1" != *"5000"* ]]
do
	echo "Wrong link mode, restarting ..."
	systemctl restart advertise5000baseT_enp4so.service
	sleep 5	
	link_mode_1=$(sudo ethtool enp4s0 | grep "Advertised link modes: " | awk '{print $4}')
done

link_mode_2=$(sudo ethtool enp9s0 | grep "Advertised link modes: " | awk '{print $4}')

echo "Advertised link modes for enp9s0:"
echo "$link_mode_2"

while [[ "$link_mode_2" != *"5000"* ]]
do
        echo "Wrong link mode, restarting ..."
        systemctl restart advertise5000baseT_enp9so.service
        sleep 5
        link_mode_2=$(sudo ethtool enp9s0 | grep "Advertised link modes: " | awk '{print $4}')
done
