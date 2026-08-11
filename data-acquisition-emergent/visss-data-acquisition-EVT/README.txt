visss-data-acquisition-EVT
============
VISSS data acquisition for Emergent Vision cameras via eSDK Pro. Started as a copy of the
eSDK Pro `record_nvenc` example. Connects to all cameras on every `-s` server using GPUDirect -
one client process manages every camera on every server it's told about (no per-camera process,
unlike the old Teledyne pipeline). This is a real deployment shape, not just a single-camera
convenience - a combined leader+follower host runs two cameras behind one server, and the Python
launcher's EmergentInstrument creates one instance (one process, one -c per camera) per
deployment, not per camera - see launch_visss_data_acquisition.py's EmergentInstrument class doc
and ../record/README.txt's _latest_0.* naming note for what broke before that was fixed. Pipeline
per camera:
CameraTask -> MotionDetectTask (../motion_detect/) -> RecordTask (../record/), the latter
replacing eSDK Pro's built-in NvencTask - see ../record/README.txt for why. See ../README.md for
the overall architecture diagram and eSDK Pro API primer.

Startup sequence (main.cpp's main())
============
1. -h/--help short-circuit, before any SDK calls.
2. System::Create(), connect to each -s server.
3. Discover + open every camera on every server; apply that camera's -c config file if any (see
   the -c entry below for the generic-parameter-loader design).
4. PTP sync: set PtpMode=TwoStep on every camera, poll PtpStatus until "Slave" (30x 1s retries),
   then pipeline.SetPtpSyncMode(true) so the pipeline itself also refuses to run if any camera
   loses sync later. This is mandatory, not a flag - a lock failure throws and aborts startup, it
   never silently falls back to free-running/unsynced capture (matching the old Teledyne
   pipeline's own §3.5 requirement). There is no "Master" role for eSDK Pro cameras - they only
   ever sync as a slave to an external grandmaster (the PTP grandmaster on the camera network, a
   Mellanox NIC in this deployment); confirmed via the vendor's own EVT_PTP example. Does NOT set
   TriggerMode/AcquisitionMode (unlike the record_nvenc example it started from) - matches the old
   VISSS leader's free-running, untriggered design.
5. Per camera: create CameraTask, MotionDetectTask, RecordTask, wire InFrame/OutFrame/ShouldWrite
   ports (plus PreviewFrame -> ImageDisplayTask if preview is on), set every task param from the
   parsed CLI flags.
6. pipeline.Start().
7. Poll loop (200ms tick): checks --maxframes, relays RecordTask segment-lifecycle events
   (currently non-functional - see the --maxframes entry below), re-polls camera temperature/PTP
   every 30s, prints a per-camera STATUS heartbeat every ~1s, pumps the cv::imshow/cv::waitKey
   event loop if preview is on.
8. On stop (Ctrl+C/SIGTERM/--maxframes reached): pipeline.Stop() - graceful, lets RecordTask
   finalize whatever segment is currently open rather than losing/corrupting the tail.

Logging convention matches the old Teledyne pipeline's exact format (PrintThread,
storage_worker_cv.h/visss-data-acquisition.h) so the shared Python launcher can parse both
binaries' output the same way: `LEVEL[-id] | timestamp | message`, left-anchored, 2-digit-year +
tenths-of-second timestamp, levels INFO/WARNING/ERROR/STATUS/BASH. Helper: LogLine(level, id,
message) in main.cpp; NowString() for the timestamp format specifically (verified byte-for-byte
against the old pipeline's get_timestamp(), not guessed).

Build
============
For building this example, open a terminal and move to this example folder, then execute the following commands:

Windows (Powershell):
- `cmake -S . -B build`
- `cmake --build build --config Release`

Linux:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`

Command Line Interface:
============
-s <server IP> <server recording path>
    Identifies a server to use. The recording path is relative to the eSDKPro server process so it must be an absolute path.
    eSDK Pro adds the current timestamp to the output directory to differentate between multiple recordings which use the same root path.
-b <bitrate Kbps>
    The target bitrate in Kbps. Default 10000.
-i <num sec>
    New-file interval: RecordTask closes the current file and starts a new one every this-many
    seconds (0 = never), no dropped frames, no pipeline restart. Default 600. See
    ../record/recordtask.h for the exact file naming/rollover mechanics.
-n <name>
    Instrument name, used in file naming (matches the old CLI's -n/--name). Default VISSS.
-e <preset>
    nvenc preset (p1 fastest/lowest-quality .. p7 slowest/highest-quality at a fixed bitrate).
    Default p1. Still being tuned against real hardware - see record/recordtask.h's Preset param
    comment for measured data points (p1: ~50% NVENC utilization, 0 dropped frames at 485fps;
    p4: ~90% utilization, dropped frames returned).
-r
    Rotate the recorded frame 90 degrees counterclockwise (matches the old CLI's
    -r/--rotateimage, PROCESSING_SPEC_teeldyne.md §3.19). Off by default. Fixed angle only, not
    configurable. Swaps width/height of the recorded video versus the raw camera frame.
-m <20|30>
    Motion-detection histogram bin-edge table selector (matches the old CLI's
    -b/--minBrightChange, §3.18). Must be 20 or 30. Default 20.
-w
    Disable recording-decision filtering (matches the old CLI's -w/--writeallframes, §3.21):
    every frame is written regardless of motion. Off by default (filtering enabled).
-l <ratio>
    Live preview decimation (matches the old CLI's -l/--liveratio, §3.25): refresh the preview
    window every this-many frames. Default 70.
--nopreview
    Disable live preview generation entirely (matches the old CLI's --nopreview). Preview is a
    real GUI window per camera (cv::imshow, requires an X session - DISPLAY must be set), matching
    the old pipeline's own preview exactly. Needs libopencv-dev at build time.
-q <count>
    Buffers pre-registered per MotionDetectTask output port - the new pipeline's equivalent of the
    old bounded frame_queue between capture and storage threads, giving Process() slack to absorb
    a momentary downstream slowdown instead of blocking immediately. Tested clean (0 dropped/
    missed) at 485fps with values as low as 1 up through p5 (see record/recordtask.h's Preset
    comment) - doesn't rescue a preset that's sustainedly too slow (p6/p7), only transient
    slowdowns. Default 8, a modest safety margin.
-c <serial> <path>
    Optional per-camera config file (repeatable, one per camera), applied to the matching camera
    right after it's opened, before PTP sync/recording starts. A camera whose serial has no
    matching -c keeps its power-on-default parameters. Plain text, one "FeatureName value" pair
    per line (e.g. "FrameRate 485"), using the camera's actual GenICam feature names - same
    format/spirit as the old Teledyne pipeline's config file. No YAML/JSON dependency - values are
    applied by probing each known camera-parameter type (UInt32/Int32/Float/Bool/Enum/String/
    Register/Command) until one matches, so new parameter names can be added to the file without
    any code changes. If ANY line fails (unknown feature, bad value), every line's result is still
    logged, but the whole program aborts startup rather than running with a silently partial/wrong
    camera configuration - fix the file and rerun.
--site <name>
    Site name shown in the status-bar overlay text (matches the old CLI's -s/--site; a long flag
    here since -s is already this file's server flag). Default "none".
--maxframes <count>
    Debug: stop the run once ~<count> frames have elapsed (matches the old CLI's -m/--maxframes;
    a long flag here since -m is already this file's MinBrightChange flag). This is an
    elapsed-time estimate (frames / camera FrameRate), not an exact live frame count - eSDK Pro
    TaskParam values set from inside a plugin's Process()/other methods never propagate back to
    the client (confirmed by testing; the same root cause as RecordTask's segment-lifecycle
    events never appearing in the console - see ../record/recordtask.h). Frames arrive at a
    steady, PTP-disciplined rate, so the estimate is accurate to a fraction of a second in
    practice. Default: unlimited (the deployment default).
-h, --help
    Print a quick-reference flag summary and exit. Checked before any startup work, so this is
    fast/side-effect-free even without cameras or a server reachable.

PTP sync is mandatory, not a flag - see "Startup sequence" above for the detail; requires a
grandmaster reachable on the camera network (e.g. the Mellanox NIC).

STATUS heartbeat (console, ~1s per camera, not a flag)
============
The client prints a `STATUS-<serial> | timestamp | frames~N | temp XC | PTP <status>` line once
per second per camera - the client-side equivalent of the old Teledyne pipeline's once-per-second
STATUS line (storage_worker_cv.h:743). Downstream tooling (the Python launcher's per-camera status
widget/Clean-button gating) depends on seeing a line starting with `STATUS` to know the pipeline
is actually alive, not just started. Can't reproduce the old line's exact content (live queue
length/histogram/move%) - those are computed entirely server-side inside the plugins' Process(),
and TaskParam values don't sync that direction (see the --maxframes entry above) - so this reports
what the client can actually observe directly: real camera temperature/PTP (the same GenICam reads
the 30s status poll already does, just more often) plus the same elapsed-time frame estimate as
--maxframes, explicitly marked with `~`.

Generic camera-param loader design (for -c above)
============
eSDK Pro has no type-erased/generic parameter setter like GenApi's CValuePtr::FromString() -
every Camera::GetParameter<T>(name) call requires the caller to already know T at compile time,
and there's no documented way to ask "what type is this parameter?" without already picking one.
GetParameter<T>() itself validates name+type and throws if wrong, though, and every GenICam node
has exactly one true type - so the loader (SetCameraParamGeneric in main.cpp) reconstructs generic
dispatch by trying each known CameraParamType in turn (UInt32, Int32, Float, Bool, Enum, String,
Register, Command) until one's GetParameter<T>() doesn't throw. This is more code than a hardcoded
parameter table, but means the config file can gain new parameter names later with zero C++
changes, matching the old pipeline's real flexibility. Numeric values are parsed with
std::stoul/std::stof (throw on bad input), not std::atoi (silently returns 0) - deliberate, since
silently applying e.g. FrameRate=0 from a typo is exactly the failure mode this feature exists to
prevent.

Eg: To record on a local server in the current directory, stopping after 1000 frames:

Windows (Powershell):
- `.\build\Release\visss-data-acquisition-EVT.exe -s 127.0.0.1 (Get-Location).Path --maxframes 1000`

Linux:
- `./build/visss-data-acquisition-EVT -s 127.0.0.01 $PWD --maxframes 1000`

Notes
============
- On Windows you may not have permissions to create/modify files in the installed example directory. If so, move this example to another location before building.
- On Windows you must add the eSdkPro bin directory to the current terminal session before running the example, for finding the necessary dependencies, e.g: `$env:PATH = "$env:PATH;$env:ECAPTURE_PRO_DIR\eSdkPro\bin"`
- PTP infrastructure (ptp4l via linuxptp, the ../scripts/services/visss_ptp_*@.service /
  visss_sync_*@.service units) is not part of this repo's C++ build - it has to actually be
  running before this binary can lock. See ../../install_commands_bullseye.md's "PTP Clock and
  NIC Configuration" section (§3a/b/c depending on leader/follower/combined topology) - this
  applies identically regardless of which camera SDK is in use.
- Periodic (30s) camera status poll reads SensTemp (Int32CameraParam, not Float - confirmed
  against this camera's real feature list, not assumed) and PtpStatus. PHYSNRMargin (considered
  as a network-link-health substitute for the old pipeline's Teledyne-only transport diagnostics)
  does NOT exist on this camera despite being in Emergent's docs ("Parameter PHYSNRMargin not
  found") - dropped rather than left failing every call. No network-link-health substitute
  currently available; RoCENackCount was flagged as a possible alternative from the vendor's
  transport-layer docs but never confirmed present - worth checking eCapture Pro's parameter
  browser if this is revisited.