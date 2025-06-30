#!/bin/bash

NIC="$1"  # Change if needed
FIVE_GBIT_MASK_HEX="0x1000000000000"
MODE="5000baseT/Full"


echo "$(date): running for $NIC"

# Check if NIC exists
if ! ip link show "$NIC" &>/dev/null; then
  echo "$(date): $NIC not found."
  exit 1
fi

ADVERTISED=$(ethtool "$NIC" | grep -A 20 "Advertised link modes:")

# Check if 5Gbit is already advertised
if echo "$ADVERTISED" | grep -q "$MODE"; then
  echo "$(date): $NIC already advertises $MODE."
  exit 0
fi

# Check if supported
SUPPORTED=$(ethtool "$NIC" | grep -A 20 "Supported link modes:")
if ! echo "$SUPPORTED" | grep -q "$MODE"; then
  echo "$(date): $NIC does not support $MODE."
  exit 1
fi

# Attempt to set 5Gbit
echo ethtool -s "$NIC" advertise "$FIVE_GBIT_MASK_HEX"
ethtool -s "$NIC" advertise "$FIVE_GBIT_MASK_HEX"
if [ $? -eq 0 ]; then
  echo "$(date): Set $NIC to advertise $MODE."
else
  echo "$(date): Failed to set $MODE on $NIC."
fi
