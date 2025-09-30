#!/bin/bash
# NIC IRQ pinning to P-cores primarily, with optional E-core fallback

# List of NICs to pin
NICS=("enp1s0f0" "enp1s0f1")

# Detect P-cores and E-cores
P_CORES=($(lscpu -e=CPU,MAXMHZ | tail -n +2 | awk '$2>=7000 {print $1}'))
E_CORES=($(lscpu -e=CPU,MAXMHZ | tail -n +2 | awk '$2<7000 {print $1}'))

echo "Detected P-cores: ${P_CORES[@]}"
echo "Detected E-cores: ${E_CORES[@]}"

# Convert CPU list to hex mask
cpu_list_to_mask() {
    local cpus=("$@")
    local mask=0
    for cpu in "${cpus[@]}"; do
        mask=$((mask | (1 << cpu)))
    done
    printf "%x" $mask
}

# Pin IRQs
for NIC in "${NICS[@]}"; do
    IRQs=($(grep -w "$NIC" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '))
    for i in "${!IRQs[@]}"; do
        IRQ=${IRQs[$i]}
        if [ $i -lt ${#P_CORES[@]} ]; then
            # Assign first IRQs to P-cores
            core_index=$(( i % ${#P_CORES[@]} ))
            MASK=$(cpu_list_to_mask ${P_CORES[$core_index]})
            echo "Pinning IRQ $IRQ ($NIC queue $i) to P-core ${P_CORES[$core_index]} (mask $MASK)"
        else
            # Remaining IRQs can go to E-cores
            core_index=$(( i % ${#E_CORES[@]} ))
            MASK=$(cpu_list_to_mask ${E_CORES[$core_index]})
            echo "Pinning IRQ $IRQ ($NIC queue $i) to E-core ${E_CORES[$core_index]} (mask $MASK)"
        fi
        echo $MASK | sudo tee /proc/irq/$IRQ/smp_affinity
    done
done

echo "Done pinning NIC queues with P-core priority and E-core fallback."
