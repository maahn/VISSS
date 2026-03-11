# Sonic Installation and Configuration Guide

## PCI Serial Card Setup (if used)

1. **Install kernel headers**:
   ```bash
   sudo apt install linux-headers-$(uname -r)
   ```

2. **Download and install ASIX driver**:
   - Download from [ASIX website](https://www.asix.com.tw/en/support/download)
   - Extract and compile:
     ```bash
     tar -jxvf AX99100_SP_PP_SPI_Linux_Driver_v1.8.0_Source.tar.bz2 
     cd AX99100_SP_PP_SPI_Linux_Driver_v1.8.0_Source/
     make
     sudo insmod ax99100.ko
     ```

3. **Verify installation**:
   ```bash
   dmesg
   ```

4. **Permanent installation**:
   ```bash
   sudo cp ax99100.ko /lib/modules/$(uname -r)/kernel/drivers/tty/serial
   sudo depmod
   ```

5. **Reboot and verify**:
   ```bash
   reboot
   lsmod
   ls /dev/ttyF0
   ```

## Always Required Steps

1. **Add user to dialout group**:
   ```bash
   sudo usermod -a -G dialout visss
   ```

2. **Enable autostart**:
   ```bash
   ln -s /home/visss/VISSS/scripts/sonic_logger.desktop /home/visss/.config/autostart/
   ```

3. **Setup cronjob for gzip compression**:
   ```bash
   crontab -e
   # Add this line:
   2 1 * * * python3 /home/visss/VISSS/sonic/sonic_gzip_files.py /data/sail/sonic
   ```
