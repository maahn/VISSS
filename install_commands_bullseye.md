# Debian Bullseye Installation and Configuration Guide

## Installation

1. Install Debian Bullseye with:
   - MATE desktop
   - SSH server
   - Webserver
   - User `visss`

2. Make sure automatic reboot after power failure is enabled in EFI settings.

## Initial Configuration

### Add user to sudoers
```bash
su
/sbin/adduser username sudo
```

### Network Configuration
- Leader network card: fixed IP `192.168.100.1`
- Follower network card: fixed IP `192.168.200.1`

### System Updates
```bash
sudo apt-get update
sudo apt-get upgrade
```

### Software Installation
Install required packages:
```bash
sudo apt install make gcc libx11-dev libxext-dev libgtk-3-dev libglade2-0 libglade2-dev libpcap0.8 libcap2 ethtool net-tools git gitk libcanberra-gtk-module libcanberra-gtk3-module libtiff-dev libboost-all-dev ffmpeg vlc apt-transport-https intltool libges-1.0-dev gstreamer1.0-plugins-bad libnotify-bin libnotify-dev ipython3 cmake-data librhash0 libuv1 cmake-qt-gui libavcodec-dev libavformat-dev libswscale-dev libdc1394-22-dev libxine2-dev libv4l-dev libgtk2.0-dev libtbb-dev libatlas-base-dev libmp3lame-dev libtheora-dev libvorbis-dev libxvidcore-dev libopencore-amrnb-dev libopencore-amrwb-dev x264 v4l-utils python3-numpy python3-dev python3-pip htop openssh-server mdm mdadm samba cifs-utils chrome-gnome-shell apache2 certbot libpcap-dev python3-tk unattended-upgrades net-tools libopencv-dev gnome-disk-utility gnome-system-tools software-properties-gtk network-manager-openconnect-gnome openconnect seahorse rsync x2goserver x2goserver-xsession gtkterm python3-pysolar python3-filelock autossh python3-pil linux-image-rt-amd64
```

### Sublime Text Installation
```bash
wget -qO - https://download.sublimetext.com/sublimehq-pub.gpg | sudo apt-key add -
echo "deb https://download.sublimetext.com/ apt/stable/" | sudo tee /etc/apt/sources.list.d/sublime-text.list 
sudo apt-get update
sudo apt-get install sublime-text
```

## GigE Framework Installation

1. Download framework from [Teledyne DALSA](https://www.teledynedalsa.com/en/support/downloads-center/software-development-kits/132/)
2. Install:
```bash
cd DALSA/
sudo ./corinstall
```
3. Reboot system

## VISSS Software Setup

1. Clone repositories:
```bash
git clone https://github.com/maahn/VISSS_configuration
git clone https://github.com/maahn/VISSS
```

2. Copy sudoers file:
```bash
sudo cp /home/visss/VISSS/scripts/visss-sudoers /etc/sudoers.d/visss
```

3. Build:
```bash
cd VISSS/data-acquisition/
make
```

4. Create application shortcuts:
```bash
mkdir -p /home/visss/.local/share/applications/
ln -s /home/visss/VISSS/scripts/visss_gui.desktop /home/visss/.local/share/applications/
```

5. Enable autostart:
```bash
mkdir -p ~/.config/autostart
ln -s /home/visss/VISSS/scripts/visss_gui.desktop /home/visss/.config/autostart/
```

## System Configuration

### Autologin Setup
1. Ensure lightdm is used:
```bash
sudo dpkg-reconfigure lightdm
```

2. Add to `/etc/lightdm/lightdm.conf`:
```bash
[SeatDefaults]
autologin-user=visss
autologin-user-timeout=0
user-session=mate
```

3. Reboot and verify

### Timezone
```bash
sudo timedatectl set-timezone UTC
```

### RAID Setup (if needed)
1. Create RAID:
```bash
sudo mdadm --create --verbose /dev/md0 --level=5 --raid-devices=3 /dev/sdb1 /dev/sdc1 /dev/sdd1
```

2. Format and setup RAID in `/data`

3. Verify after reboot:
```bash
lsblk
```

### Apache2 Web Server
1. Configure document root:
```bash
sudo nano /etc/apache2/sites-enabled/000-default.conf
```
Add:
```apache
DocumentRoot /data
```

2. Configure directory settings:
```bash
sudo nano /etc/apache2/apache2.conf
```
Add:
```apache
<Directory /data/>
    Options Indexes FollowSymLinks
    AllowOverride None
    Require all granted
    EnableMMAP Off
</Directory>
```

3. Restart service:
```bash
sudo systemctl restart apache2
```

### Automatic Updates
1. Enable security updates:
```bash
sudo software-properties-gtk
```

2. Exclude kernel upgrades in `/etc/apt/apt.conf.d/50unattended-upgrades`:
```bash
Unattended-Upgrade::Package-Blacklist {
    "linux-headers*";
    "linux-image*";
    "linux-generic*";
    "linux-modules*";
}
```

## System Services

### PTP Clock and NIC Configuration
1. Link config file:
```bash
ln -s /home/visss/visss_config/visss2/VISSS_INTERFACES.env VISSS_INTERFACES.env
```

2. Install services:
```bash
sudo cp -v ~/VISSS/scripts/services/* /etc/systemd/system/
sudo systemctl daemon-reexec
```

3. Enable services for leader:
```bash
sudo systemctl enable --now visss_ptp_master@LEADER_NIC_VISSS
sudo systemctl enable --now visss_sync_systemclock_to@LEADER_NIC_VISSS
```

4. Enable services for follower:
```bash
sudo systemctl enable --now visss_ptp_master@FOLLOWER_NIC_VISSS
sudo systemctl enable --now visss_ptp_slave@FOLLOWER_NIC_INTERNET
```

## Additional Setup

### VPN Configuration (Deprecated)
1. Create polkit file:
```bash
sudo nano /etc/polkit-1/localauthority/10-vendor.d/org.freedesktop.NetworkManager.pkla
```
Add:
```bash
[nm-applet]
Identity=unix-user:visss
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=no
ResultActive=yes
```

2. Create VPN script:
```bash
#!/bin/bash
CON="vpn.uni-leipzig.de"
STATUS=`nmcli con show --active | grep $CON | cut -f1 -d " "`
if [ -z "$STATUS" ]; then
    nmcli con up $CON
fi
```

3. Add to cron:
```bash
crontab -e
```
Add:
```bash
* * * * * /home/visss/vpn_reconnect.sh > /home/visss/vpn_reconnect.log 2>&1
```

### Git Access
1. Generate SSH key:
```bash
ssh-keygen -t ed25519 -C "your email"
cat /home/visss/.ssh/github_id_ed25519.pub
```

2. Add key to GitHub repository settings.

3. Set Git remote:
```bash
git remote set-url origin git@github.com:maahn/VISSS.git
```

## Verification

1. Check services status:
```bash
sudo systemctl status visss_ptp_master@LEADER_NIC_VISSS
sudo systemctl status visss_sync_systemclock_to@LEADER_NIC_VISSS
```

2. Verify RAID status:
```bash
sudo mdadm --detail /dev/md0
```

3. Check system time:
```bash
timedatectl status
```

4. Verify Apache configuration:
```bash
sudo apache2ctl -t
```

You can view the updated instructions with:
```bash
xdg-open install_commands_bullseye.md
```

install_commands_bullseye.txt
```text
<<<<<<< SEARCH
Install Debian Bullseye with MATE desktop, ssh server, webserver and user visss
***********
Make sure automatic reboot after power failure is enabled in EFI. Look for "Restore on AC Power Loss" or  "Power On After Power Loss" or "AC Recovery"

**********

add visss user to sudoer:
su
/sbin/adduser username sudo

configure network cards
network card for leader fixed ip 192.168.100.1
network card for follower fixed ip 192.168.200.1

***********


sudo apt-get update
sudo apt-get upgrade
sudo apt install make  gcc  libx11-dev libxext-dev libgtk-3-dev libglade2-0 libglade2-dev libpcap0.8 libcap2 ethtool  net-tools  git gitk libcanberra-gtk-module libcanberra-gtk3-module libtiff-dev libboost-all-dev ffmpeg vlc apt-transport-https intltool  libges-1.0-dev libges-1.0-dev gstreamer1.0-plugins-bad libnotify-bin libnotify-dev ipython3 cmake-data librhash0 libuv1 cmake-qt-gui  libavcodec-dev libavformat-dev libswscale-dev libdc1394-22-dev libxine2-dev libv4l-dev libgtk2.0-dev libtbb-dev libatlas-base-dev libmp3lame-dev libtheora-dev libvorbis-dev libxvidcore-dev libopencore-amrnb-dev libopencore-amrwb-dev x264 v4l-utils  python3-numpy python3-dev python3-pip htop openssh-server mdm mdadm  samba cifs-utils  chrome-gnome-shell apache2 certbot  libpcap-dev python3-tk unattended-upgrades net-tools libopencv-dev gnome-disk-utility gnome-system-tools software-properties-gtk network-manager-openconnect-gnome openconnect seahorse rsync x2goserver x2goserver-xsession gtkterm python3-pysolar python3-filelock autossh python3-pil linux-image-rt-amd64 

wget -qO - https://download.sublimetext.com/sublimehq-pub.gpg | sudo apt-key add -
echo "deb https://download.sublimetext.com/ apt/stable/" | sudo tee /etc/apt/sources.list.d/sublime-text.list 
sudo apt-get update
sudo apt-get install sublime-text



*********************
download gig e v framework
https://www.teledynedalsa.com/en/support/downloads-center/software-development-kits/132/
of from Speicherwolke VISSS folder!

cd DALSA/
sudo ./corinstall 

reboot!


#### VISSSS Software #################
cd
git clone https://github.com/maahn/VISSS_configuration

cd
git clone https://github.com/maahn/VISSS
cd VISSS/data-acquisition/
sudo cp /home/visss/VISSS/scripts/visss-sudoers /etc/sudoers.d/visss

make

If make fails, try "source /etc/profile"


mkdir -p /home/visss/.local/share/applications/
ln -s /home/visss/VISSS/scripts/visss_gui.desktop /home/visss/.local/share/applications/



launch visss_GUI in /home/visss/Desktop/VISSS/scripts/  to sign them

drag visss_GUI to menu bar for shortcut


mkdir -p ~/.config/autostart
ln -s /home/visss/VISSS/scripts/visss_gui.desktop /home/visss/.config/autostart/


********* autologin without Password
make sure lightdm is the used display manager with:

sudo dpkg-reconfigure lightdm

then, add to /etc/lightdm/lightdm.conf

[SeatDefaults]
autologin-user=visss
autologin-user-timeout=0
user-session=mate

don't forget to reboot to test whether it works!

### timezone

sudo timedatectl set-timezone UTC

####### AUTOSSH

If required for data transfer, see Monsun wiki for service script

###### VPN (DEPRECEATED USE AUTOSSH)

configure vpn with name vpn.uni-leipzig.de

add file "/etc/polkit-1/localauthority/10-vendor.d/org.freedesktop.NetworkManager.pkla" with:
--
[nm-applet]
Identity=unix-user:visss
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=no
ResultActive=yes
--

create VPN script  "/home/visss/vpn_reconnect.sh" with:
--
#!/bin/bash
CON="vpn.uni-leipzig.de"
STATUS=`nmcli con show --active | grep $CON | cut -f1 -d " "`
if [ -z "$STATUS" ]; then
    nmcli con up $CON
fi
--

do "chmod +x /home/visss/vpn_reconnect.sh" and add to cron with "crontab -e" and add: 
--
* * * * * /home/visss/vpn_reconnect.sh  > /home/visss/vpn_reconnect.log 2>&1
--

finally remove (ie set it to blank) the password of the login key ring (using seahorse), it needs to be unlocked for the vpn password       

##### apache2 web server for minitoring #####

change directory in /etc/apache2/sites-enabled/000-default.conf
        DocumentRoot /data

add to /etc/apache2/apache2.conf
--
<Directory /data/>
	Options Indexes FollowSymLinks
	AllowOverride None
	Require all granted
      EnableMMAP Off
</Directory>
--

sudo systemctl restart apache2

#### automatic updates

automatic security updates can be enabled by software-properties-gtk

but make sure to exclude kernel upgrades becuase they mess with the serial card drivers:
sudo nano /etc/apt/apt.conf.d/50unattended-upgrades

Unattended-Upgrade::Package-Blacklist {
      "linux-headers*";
      "linux-image*";
      "linux-generic*";
      "linux-modules*";
}






####create RAID (if several HDD are installed) #####

#use partitions and make them a little smaller becuase disk sizes are not always the same which can cause problems when swapping disks

sudo mdadm --create --verbose /dev/md0 --level=5 --raid-devices=3 /dev/sdb1 /dev/sdc1 /dev/sdd1
use GUI to fromat and setup RAID in /data
check whetehr RAID exists after reboot




####System d scripts


The scripts ensure that the 5Gbit mode is enabled for the VISSS network cards (NIC) and that clock sysncing works with PTP. It is a rather complicated setup. For the leader (and follower when runnign on the same computer):
leader system clock -> leader NIC VISSS clock -> VISSS leader camera clock
For the follower:
leader system clock -> leader NIC Internet clock -> follower NIC Internet clock -> follower NIC VISSS clock -> VISSS leader follower clock


#link and edit config file, e.g.:
ln -s /home/visss/visss_config/visss2/VISSS_INTERFACES.env VISSS_INTERFACES.env
# This is crucial, otherwise nothing will work!


#### Services
# for setting up PTP clocks and making sure 5gbit is advertised by NIC 

#adapt and link config file, e.g.:
ln -s /home/visss/visss_config/visss3/VISSS_INTERFACES.env VISSS_INTERFACES.env
# this is very important, otherwise nothing works

#install service scripts with:
sudo cp -v ~/VISSS/scripts/services/* /etc/systemd/system/
sudo systemctl daemon-reexec

#make sure visss user can see logs:
sudo usermod -aG systemd-journal visss

#for single combined leader&follower:
sudo systemctl enable --now visss_ptp_master@LEADER_NIC_VISSS
sudo systemctl enable --now visss_ptp_master@FOLLOWER_NIC_VISSS
sudo systemctl enable --now visss_sync_systemclock_to@LEADER_NIC_VISSS
sudo systemctl enable --now visss_sync_systemclock_to@FOLLOWER_NIC_VISSS

sudo apt install chrony #make sure sytem time is alway right


###for leader:
sudo systemctl enable --now visss_sync_systemclock_to@LEADER_NIC_VISSS
sudo systemctl enable --now visss_sync_systemclock_to@LEADER_NIC_INTERNET
sudo systemctl enable --now visss_ptp_master@LEADER_NIC_VISSS
sudo systemctl enable --now visss_ptp_master@LEADER_NIC_INTERNET

sudo systemctl status visss_sync_systemclock_to@LEADER_NIC_VISSS
sudo systemctl status visss_sync_systemclock_to@LEADER_NIC_INTERNET
sudo systemctl status visss_ptp_master@LEADER_NIC_VISSS
sudo systemctl status visss_ptp_master@LEADER_NIC_INTERNET

#for Nano 5G camera only, runs every minute
sudo systemctl enable --now visss_check-5gbit@LEADER_NIC_VISSS.timer
sudo systemctl status visss_check-5gbit@LEADER_NIC_VISSS.service

#make sure sytem time is always right
sudo apt install chrony

###for follower:
sudo systemctl enable --now visss_ptp_master@FOLLOWER_NIC_VISSS
sudo systemctl enable --now visss_ptp_slave@FOLLOWER_NIC_INTERNET
sudo systemctl enable --now visss_sync_nic_clock_to@FOLLOWER_NIC_VISSS
sudo systemctl enable --now visss_sync_nic_clock_to_systemclock

sudo systemctl status visss_ptp_master@FOLLOWER_NIC_VISSS
sudo systemctl status visss_ptp_slave@FOLLOWER_NIC_INTERNET
sudo systemctl status visss_sync_nic_clock_to@FOLLOWER_NIC_VISSS
sudo systemctl status visss_sync_nic_clock_to_systemclock

#for the follower only, make sure chrony and ntp are not running
ps -ef | grep -E 'chronyd|ntpd|timesyncd'

#for Nano 5G camera only, runs every minute
sudo systemctl enable --now visss_check-5gbit@FOLLOWER_NIC_VISSS.timer
sudo systemctl status visss_check-5gbit@FOLLOWER_NIC_VISSS.service



##### git access

create git access with
ssh-keygen -t ed25519 -C "your email"
cat /home/visss/.ssh/github_id_ed25519.pub

add key to repo → Settings → Deploy keys

git remote set-url origin git@github.com:maahn/VISSS.git
