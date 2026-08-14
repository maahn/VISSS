# Install commands — Emergent Vision / eSDK Pro stack (Ubuntu 22.04)

Mirrors `install_commands_bookworm.txt`/`install_commands_bullseye.md`'s format for the old
Teledyne stack, but for the Emergent Vision camera port in `data-acquisition-emergent/`.

**Status**: reconstructed from the actual installed state of the dev/test box
(`microvisss`, Ubuntu 22.04.5) on 2026-08-10, not written live during the original install. Where
the original install steps/choices aren't recoverable from the box's current state, the heading is
left with a note instead of being guessed at — fill in from memory or the vendor docs before this
is used to provision a new machine.

## Hardware / BIOS

(Not documented — fill in. See `install_commands_bookworm.txt` for the old stack's equivalent
notes: enable auto-reboot after power failure in EFI, etc. Likely still applicable.)

Confirmed present on the dev box: NVIDIA RTX PRO 4000 Blackwell GPU, dual-port Mellanox ConnectX-6
Lx NIC (`enp129s0f0np0`/`enp129s0f1np1` — a "combined leader+follower" dev topology, NOT the
two-PC production split), Emergent HR-2000SM camera(s).

## OS Install

Ubuntu 22.04.5 LTS (Jammy), kernel via `linux-generic-hwe-22.04` (HWE kernel, not the default GA
kernel — needed for current Mellanox/NVIDIA driver support). Desktop flavor/install-media choices,
partitioning scheme, etc. not documented — fill in.

```bash
sudo apt update
sudo apt upgrade
```

## NVIDIA GPU driver + CUDA

Driver: `nvidia-driver-595-open` (open-source kernel module variant — required for GPUDirect/newer
GPU generations on recent driver branches; the closed-source `nvidia-driver-595` was NOT what's
installed here). CUDA toolkit 12.9 (`nvcc --version` → `V12.9.86`). Secure-boot-related packages
present (`mokutil`, `shim-signed`) — if secure boot is enabled, the NVIDIA kernel modules need MOK
enrollment; exact steps not documented here.

```bash
sudo apt install nvidia-driver-595-open
# CUDA 12.9 toolkit install: exact method (runfile vs. apt repo) not documented - fill in.
# Confirm with:
nvidia-smi
nvcc --version
```

## Mellanox / NVIDIA DOCA networking stack

Installed via NVIDIA's DOCA host package (**not** plain `apt install` from Ubuntu's repos — this
is a large vendor bundle providing MLNX_OFED kernel modules via DKMS, RDMA userspace libraries,
and DOCA SDK/tools). The installed `doca-host` .deb sets up a **local file-based apt repo**
pointing at its own bundled package pool (confirmed: `/etc/apt/sources.list.d/doca.list` points at
`file:/usr/share/doca-host-3.3.0-088000-26.01-ubuntu2204/repo`), then `doca-all`/`doca-host` (and
their many `doca-sdk-*`/`libdoca-*` dependencies, ~60 packages, all pulled in automatically - not
individually apt-installed) get installed from that local repo.

```bash
# Download the DOCA host package for Ubuntu 22.04 from NVIDIA's DOCA downloads page
# (exact URL/version used here not recorded - installed version: 3.3.0-088000, OFED 26.01).
sudo dpkg -i doca-host_<version>_amd64.deb
sudo apt update
sudo apt install doca-host doca-all
```

DKMS modules confirmed built for the running kernel (`6.8.0-136-generic`): `mlnx-ofed-kernel`,
`kernel-mft-dkms`, `iser`, `isert`, `srp`, `xpmem` (all `26.01.OFED.26.01.1.0.0.1`/`4.35.0.159` per
`dkms status`). A reboot is normally required after this install for the kernel modules to load.

NVIDIA Rivermax SDK (`rivermax` package, `1:1.81.21`) is also required — used internally by
eCapture Pro's GPUDirect streaming path (confirmed via `strings`/`strace` this session: it reads a
license at `/var/lib/EVT/rivermax.lic` and names an internal thread `rivermax-high`). Exact
install source for the `rivermax` .deb not documented - likely bundled with or alongside the DOCA
host package, or a separate NVIDIA Rivermax SDK download.

## Emergent eSDK Pro / eCapture Pro

Installed via the vendor's self-extracting installer (confirmed present on this box at
`/home/visss/EVT/eCaptureProInstaller_1_6_1_Ubuntu_22_04/eCaptureProInstaller_1_6_1_Ubuntu_22_04.run`,
an older `1_5_0` copy also present under `~/eCapturePro/` and `/data/eCapturePro/` from a prior
version - eCapture Pro version installed on this box: **1.6.1**, eSDK compile/runtime version
**4.07.01/02**). Per `/opt/EVT/eCapturePro/README.txt`:

```bash
sudo apt install libxcb-xinerama0   # installer dependency
chmod +x eCaptureProInstaller_1_6_1_Ubuntu_22_04.run
./eCaptureProInstaller_1_6_1_Ubuntu_22_04.run
# "Install all components for a complete installation", per the vendor README.
# Log out or restart to complete installation.
```

Installs to `/opt/EVT` and registers the `emergent-esdk-ecapture` dpkg package (version `1.0`,
locally installed - not from an apt repo, so it won't show a "Candidate" in `apt-cache policy`).
Also installs:
- `/etc/profile.d/evt.sh` — sets `EMERGENT_DIR=/opt/EVT`, `RIVERMAX_LOG_LEVEL=6`, `VMA_TRACELEVEL=0`
  in every login shell. **Non-login shells (e.g. a plain CI/agent Bash tool) don't source
  `/etc/profile.d/`** — `build_and_install.sh` in this repo works around that by defaulting
  `EMERGENT_DIR`/`ECAPTURE_PRO_DIR` itself rather than requiring `source /etc/profile` first.
- systemd units `evt_alloc_hugepages.service`, `evt_bcm_init.service`, `evt_mellanox_init.service`,
  `evt_eCaptureProServer.service` (see below).
- `/etc/init.d/start-mva`, `/etc/init.d/start-nvidia-peermem` (legacy init.d scripts, not systemd).

### systemd units and their current enabled state on this box

```
evt_eCaptureProServer.service   enabled   # the capture/plugin-host daemon (see below)
evt_mellanox_init.service       enabled   # NIC/Rivermax license init at boot
evt_alloc_hugepages.service     disabled  # NOT currently used on this box
evt_bcm_init.service            disabled  # NOT currently used on this box (Broadcom NIC init -
                                           # this box uses Mellanox, not Broadcom, for the camera NIC)
```

`evt_eCaptureProServer.service` currently runs **as root** (vendor default: no `User=` directive
in the unit, no capabilities set). A same-session investigation (2026-08-10) into running it as an
unprivileged user found two concrete `RLIMIT_RTPRIO`/`RLIMIT_MEMLOCK`-related `EPERM` failures
during camera-stream open, fixed those via a systemd drop-in
(`AmbientCapabilities=`/`LimitRTPRIO=`/`LimitMEMLOCK=`), but the camera stream still failed to open
identically afterward with the same generic `EVT_CameraOpenStream returned 13` error — root cause
not identified before the investigation was stopped and all changes reverted. **Currently
unresolved; a support ticket to Emergent Vision covering the exact diagnostic trail is pending.**
If a non-root deployment mode is ever confirmed working, update this section and
`scripts/services/` accordingly - don't assume root is required without re-checking this note.

## APT packages — manually installed on this box

Captured via `apt-mark showmanual` on 2026-08-10 (not a curated "what VISSS needs" list — this is
every package apt considers manually-installed on this specific box right now, which includes
general desktop/Ubuntu packages that predate or are unrelated to VISSS, e.g. `rustdesk`,
`timeshift`, `vlc`, `mokutil`. Treat generously; don't assume every line here is required).

```bash
sudo apt install \
  autossh base-passwd bsdutils btrfs-progs build-essential cmake curl dash diffutils \
  doca-all doca-host efibootmgr emergent-esdk-ecapture ffmpeg findutils fonts-indic \
  freeglut3 freeglut3-dev gcc-12 git gitk grep grub2-common grub-common grub-efi-amd64-bin \
  grub-efi-amd64-signed grub-gfxpayload-lists grub-pc grub-pc-bin gzip hostname htop init \
  ipython3 language-pack-en language-pack-en-base language-pack-gnome-en \
  language-pack-gnome-en-base libavcodec-dev libavformat58 libavformat-dev libavutil-dev \
  libdebconfclient0 libflashrom1 libftdi1-2 libllvm13 libnuma1 libopencv-dev libxcb-cursor0 \
  linux-generic-hwe-22.04 linuxptp login mokutil ncurses-base ncurses-bin net-tools \
  nvidia-driver-595-open os-prober python3.10-venv python3-filelock python3-numpy \
  python3-pil python3-pip python3-pysolar python3-tk qml-module-qtquick2 \
  qml-module-qtquick-controls2 qml-module-qtquick-layouts qml-module-qtquick-window2 \
  qtbase5-dev qtdeclarative5-dev rivermax rustdesk screen shim-signed smartmontools ssh \
  timeshift ubuntu-desktop ubuntu-desktop-minimal ubuntu-minimal ubuntu-standard \
  ubuntu-wallpapers vlc
```

Packages directly relevant to building/running this repo's C++ side: `build-essential`, `cmake`,
`gcc-12`, `git`, `libavcodec-dev`/`libavformat-dev`/`libavutil-dev` (NVENC/HEVC muxing, see
`record/README.txt`), `libopencv-dev` (live preview, `cv::imshow`), `doca-all`/`doca-host`/
`emergent-esdk-ecapture`/`rivermax` (the vendor stack, see above), `linuxptp` (PTP daemon, see
`scripts/services/`), `nvidia-driver-595-open`.

Packages directly relevant to the Python launcher (`launch_visss_data_acquisition.py`):
`python3-tk` (Tkinter GUI), `python3-pysolar` (sun-altitude gating), `python3-filelock`
(single-instance lock), `python3-pil`/`python3-numpy` (wiper brightness check), `python3-pip`,
`screen` (per `scripts/visss_gui.desktop`'s `Exec=screen -d -m -S visss_gui ...`).

**Not apt-installed — pip-installed instead** (`python3-serial`/pyserial has no matching apt
package pulled in here; installed via pip this session):
```bash
pip install pyserial
```

## Python launcher — other setup

```bash
# ~/.visss.yaml (GUI settings/config-file pointer) is written by the app itself on first run,
# not hand-authored - see GUI.__init__'s DEFAULTGUI/read_settings in
# launch_visss_data_acquisition.py. Point it at a real deployment YAML:
cat > ~/.visss.yaml << 'YAML'
configFile: /path/to/your/deployment.yaml
autopilot: false
YAML
```

Deployment YAML schema (top-level `sdk: teledyne|emergent`, per-camera `camera:` list,
`emergentparameters`/`teledyneparameters`, wiper/externalTrigger config, etc.) - see
`launch_visss_data_acquisition.py`'s `EmergentInstrument`/`runCpp` class doc comments for the
authoritative field list; not duplicated here to avoid drift.

## PTP clock / NIC configuration

Not re-documented here in full - follow `install_commands_bullseye.md`'s "System Services → PTP
Clock and NIC Configuration" section (§3a "combined leader and follower computer" matches this
dev box's topology; a real two-PC deployment would use §3b/§3c instead). The same
`scripts/services/*` systemd units apply regardless of which camera SDK is in use - PTP setup is
fully independent of the Teledyne/Emergent choice.

## Building and running the VISSS EVT client

See `data-acquisition-emergent/README.md` and the root `AI.md` for the authoritative
build/run instructions (`./build_and_install.sh`, `EMERGENT_DIR`/`ECAPTURE_PRO_DIR` env vars,
plugin install steps) - not duplicated here.

## sudoers configuration (for the Python launcher / this session's tooling)

```
# /etc/sudoers.d/visss-ecaptureproserver
visss ALL=(root) NOPASSWD: /usr/bin/systemctl restart evt_eCaptureProServer.service
visss ALL=(root) NOPASSWD: /usr/bin/systemctl status evt_eCaptureProServer.service
visss ALL=(root) NOPASSWD: /usr/bin/systemctl stop evt_eCaptureProServer.service
```
(Reconstructed from `sudo -l` output - the actual file is root-readable-only, `-r--r-----`, so its
exact formatting wasn't read directly.) Lets `build_and_install.sh` and the Python launcher restart
the capture server without an interactive password prompt, while keeping broader `sudo` access
gated behind the normal password (this account is also in the `sudo` group with full `(ALL:ALL)
ALL` access - the NOPASSWD lines are an additional narrow carve-out on top of that, not a
replacement for it).
