#include <eSdkPro/system.h>
#include <eSdkPro/task.h>

#include <opencv2/highgui.hpp>

#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

using namespace eSdkPro;

// Set by the signal handler (must stay async-signal-safe: no SDK calls, just this flag), checked
// by the polling loop in main() which does the actual pipeline.Stop(). Ctrl+C (SIGINT) and
// SIGTERM (e.g. from a process supervisor) both trigger a clean shutdown - RecordTask::Stop()
// flushes and finalizes whatever segment is currently open (see recordtask.h) rather than losing
// the tail of the recording.
std::atomic<bool> g_shouldStop{false};

void SignalHandler(int /*signum*/)
{
    g_shouldStop.store(true);
}

// Matches the old pipeline's get_timestamp() exactly (visss-data-acquisition.h): 2-digit year,
// local time, one decimal digit of sub-second precision rounded to the nearest tenth (not
// milliseconds) - confirmed against the old source, not guessed. Previously this used a 4-digit
// year and no sub-second component at all, which didn't actually match.
std::string NowString()
{
    struct timeval tv
    {
    };
    gettimeofday(&tv, nullptr);

    int tenths = static_cast<int>(std::lround(tv.tv_usec / 100000.0));
    if (tenths >= 10)
    {
        tenths -= 10;
        tv.tv_sec++;
    }

    std::tm tmBuf{};
    localtime_r(&tv.tv_sec, &tmBuf);
    char buf[32];
    strftime(buf, sizeof(buf), "%y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf) + "." + std::to_string(tenths);
}

// Matches the old pipeline's PrintThread convention exactly (storage_worker_cv.h/
// visss-data-acquisition.h): "LEVEL | timestamp | message" for general messages, or
// "LEVEL-id | timestamp | message" for messages about one specific camera/task - level is one of
// INFO/WARNING/ERROR/DEBUG/FATAL, all left-anchored so a downstream Python parser can dispatch on
// line prefix the same way launch_visss_data_acquisition.py already does for the old binary.
std::string LogLine(const std::string& level, const std::string& id, const std::string& message)
{
    std::string prefix = level;
    if (!id.empty())
    {
        prefix += "-" + id;
    }
    return prefix + " | " + NowString() + " | " + message;
}

// Live preview (§3.25), matching the old pipeline's cv::imshow exactly rather than something
// bespoke - this box has a real X session (confirmed: gdm-x-session on :0), so a real window is
// viable, unlike the earlier "no GUI, write a file instead" assumption.
//
// The ImageDisplayTask callback (see main()'s pipeline setup) fires on a background thread, one
// per camera, and these can fire concurrently (confirmed by testing during the earlier
// file-writing version of this feature: two cameras' callbacks interleaved). GTK-backed OpenCV
// highgui is not safe to call from arbitrary/multiple threads - all cv::imshow/cv::waitKey calls
// must happen consistently from one thread. So callbacks only update a mutex-guarded "latest
// frame per camera" map; the actual cv::imshow/cv::waitKey calls happen from the main poll loop
// in main(), which already runs on a single, consistent thread for the whole program.
std::mutex g_previewMutex;
std::map<std::string, cv::Mat> g_previewFrames; // keyed by camera serial (deviceId)

void OnPreviewFrame(const std::string& deviceId, const eSdkPro::Frame& frame)
{
    // 4 bytes/pixel: ImageDisplayTask delivers frames as a display-ready 4-channel format
    // regardless of the source's MONO8 origin (confirmed by testing: GetDataSize() ==
    // width*height*4). Channel order (rgba vs. bgra/etc) isn't verified, but doesn't matter
    // visually for this grayscale-sourced content - R/G/B end up equal either way, and cv::imshow
    // treats a 4-channel Mat as BGRA regardless.
    const cv::Mat view(static_cast<int>(frame.GetHeight()), static_cast<int>(frame.GetWidth()), CV_8UC4,
                       frame.GetDataPtr(), frame.GetStride());

    std::lock_guard<std::mutex> lock(g_previewMutex);
    // .clone(): the eSdkPro::Frame's underlying buffer is only valid for the duration of this
    // callback (reused/overwritten on the next preview frame), so the map must own its own copy.
    g_previewFrames[deviceId] = view.clone();
}

// NVEnc requires a GPU to be used.
const int c_gpuId = 0;
const bool c_useGpuDirect = true;

// MotionDetectTask is loaded by eCaptureProServer from a separate .so (../motion_detect/), not
// linked into this client - these must match the MotionDetect namespace in
// motion_detect/src/motiondetecttask.h.
const std::string c_motionDetectTaskName = "MotionDetect";
const std::string c_motionDetectInputName = "InFrame";
const std::string c_motionDetectOutputName = "OutFrame";
const uint32_t c_motionDetectBorderHeight = 64;
const std::string c_motionDetectRotateParamName = "Rotate";
const std::string c_motionDetectMinBrightChangeParamName = "MinBrightChange";
const std::string c_motionDetectWriteAllFramesParamName = "WriteAllFrames";
const std::string c_motionDetectFramerateParamName = "Framerate";
const std::string c_motionDetectShouldWriteOutputName = "ShouldWrite";
const std::string c_motionDetectPreviewOutputName = "PreviewFrame";
const std::string c_motionDetectLiveRatioParamName = "LiveRatio";
const std::string c_motionDetectNoPreviewParamName = "NoPreview";
const std::string c_motionDetectQueueDepthParamName = "QueueDepth";
const std::string c_motionDetectSiteParamName = "Site";
const std::string c_motionDetectNameParamName = "Name";
const std::string c_motionDetectNewFileIntervalSecParamName = "NewFileIntervalSec";

// RecordTask replaces NvencTask (see ../record/README.txt for why) - loaded the same way as
// MotionDetectTask, must match the Record namespace in record/src/recordtask.h.
const std::string c_recordTaskName = "Record";
const std::string c_recordInputName = "InFrame";
const std::string c_recordShouldWriteInputName = "ShouldWrite";
const std::string c_recordOutputRootParamName = "OutputRoot";
const std::string c_recordNameParamName = "Name";
const std::string c_recordDeviceIdParamName = "DeviceId";
const std::string c_recordWidthParamName = "Width";
const std::string c_recordHeightParamName = "Height";
const std::string c_recordFramerateParamName = "Framerate";
const std::string c_recordBitrateKbpsParamName = "BitrateKbps";
const std::string c_recordNewFileIntervalSecParamName = "NewFileIntervalSec";
const std::string c_recordPresetParamName = "Preset";
const std::string c_recordMinBrightChangeParamName = "MinBrightChange";
const std::string c_recordTemperatureParamName = "Temperature";
const std::string c_recordPtpStatusParamName = "PtpStatus";
const std::string c_recordCameraConfigNameParamName = "CameraConfigName";
// Four separate event params, not one - see recordtask.h for why (events that fire together
// within one Process() call would otherwise coalesce before the client's next poll).
const std::vector<std::string> c_recordEventParamNames = {"LastSessionEvent", "LastSegmentClosed",
                                                           "LastSegmentStarted", "LastSnapshot"};

struct ServerParams
{
    ServerParams(const std::string& ip, const std::string& recordPath) : m_ip{ip}, m_recordPath{recordPath}
    {}

    std::string m_ip;
    std::string m_recordPath;
};

// Per-camera config file assignment (-c <serial> <path>, repeatable) - mirrors ServerParams/-s:
// leader and follower cameras don't share settings (different exposure/gain etc.), so the config
// file has to be selectable per physical camera rather than applied identically to every camera
// this process manages.
struct CameraConfigParams
{
    CameraConfigParams(const std::string& serial, const std::string& path) : m_serial{serial}, m_path{path}
    {}

    std::string m_serial;
    std::string m_path;
};

// Per-camera display-name override (--name <serial> <name>, repeatable) - mirrors
// CameraConfigParams/-c: several cameras behind one process (e.g. a combined leader+follower
// deployment) each need their OWN name baked into their OWN overlay text/output filenames
// (RecordTask's "{Name}_{DeviceId}_latest_0.*" symlinks, motion_detect's status-bar text), not the
// single shared -n value every camera got before this existed - see getCameraName().
struct CameraNameParams
{
    CameraNameParams(const std::string& serial, const std::string& name) : m_serial{serial}, m_name{name}
    {}

    std::string m_serial;
    std::string m_name;
};

struct Params
{
    std::vector<ServerParams> m_serverParams;
    uint32_t m_bitrateKbps{10000};
    // Matches old pipeline's -i/--newfileinterval; default changed from the old 300s to 600s per
    // project owner.
    uint32_t m_newFileIntervalSec{600};
    // Matches old pipeline's -n/--name; used in file naming (see record/recordtask.h).
    std::string m_name{"VISSS"};
    // nvenc preset - see record/recordtask.h's Preset param comment for the current p1-p7
    // measurement (p1-p5 clean at 485fps, p6/p7 overload). p1 kept as the default here since
    // raising it is a user decision.
    std::string m_preset{"p1"};
    // Matches the old CLI's -r/--rotateimage (PROCESSING_SPEC_teeldyne.md §3.19): fixed 90-degree
    // counterclockwise rotation, off by default.
    bool m_rotate{false};
    // Matches the old CLI's -b/--minBrightChange (§3.18): must be 20 or 30.
    int32_t m_minBrightChange{20};
    // Matches the old CLI's -w/--writeallframes (§3.21): disables recording-decision filtering.
    bool m_writeAllFrames{false};
    // Matches the old CLI's -l/--liveratio (§3.25): preview a frame every this-many frames.
    uint32_t m_liveRatio{70};
    // Matches the old CLI's --nopreview (§3.25): disables live preview generation entirely.
    bool m_noPreview{false};
    // Buffers pre-registered per MotionDetectTask output port - the new pipeline's equivalent of
    // the old bounded frame_queue between capture and storage. No old-CLI equivalent flag (the
    // old frame_queue's 3000-frame depth was hardcoded, not tunable). 8 chosen as a modest safety
    // margin over the 1-4 range tested clean at 485fps p1-p5 (see recordtask.h's Preset comment) -
    // not 1000-ish like the old frame_queue, which needed that depth for a CPU-bound
    // encoder+motion-detection pipeline prone to scheduling contention; this pipeline's GPU-fused
    // motion detection and hardware NVENC encode don't have that failure mode (confirmed by
    // testing: QueueDepth 1 vs. 4 made no difference to whether p4/p7 dropped frames).
    int32_t m_queueDepth{8};
    // Optional per-camera plain-text "FeatureName value" config files (-c <serial> <path>,
    // repeatable), applied right after each matching camera's Camera::Open() - see
    // ApplyCameraConfigFile. A camera whose serial isn't listed here keeps its power-on-default
    // parameters (unchanged fallback behavior).
    std::vector<CameraConfigParams> m_cameraConfigs{};
    // Optional per-camera display-name overrides (--name <serial> <name>, repeatable) - see
    // CameraNameParams/getCameraName(). A camera whose serial isn't listed here falls back to
    // m_name (the shared -n value), unchanged fallback behavior for single-camera-per-process
    // deployments.
    std::vector<CameraNameParams> m_cameraNames{};
    // Matches the old CLI's -s/--site: site name shown in the status-bar overlay text (long flag
    // here since -s is already this file's server flag).
    std::string m_site{"none"};
    // Matches the old CLI's -m/--maxframes debug stop-after-N-frames (long flag here since -m is
    // already this file's MinBrightChange flag). 0 = unlimited (default). Implemented as an
    // elapsed-time estimate (frames / camera FrameRate), NOT a true live frame count: eSDK Pro
    // TaskParam values set from inside a plugin's Process()/other methods never propagate back to
    // the client's GetParameter<T>().GetValue() (confirmed by testing - this is the same root
    // cause as the separately-known "RecordTask console event relay not printing" gap, not a new
    // issue). Frames arrive at a steady, PTP-disciplined rate, so this estimate is accurate to a
    // fraction of a second in practice - see the main() poll loop.
    uint32_t m_maxFrames{0};
};

// Full flag reference - keep in sync with README.txt's "Command Line Interface" section (which
// has the longer rationale/detail for each flag; this is the quick-reference version).
void PrintHelp(const std::string& progName)
{
    std::cout << "Usage: " << progName << " -s <server ip> <record path> [options]\n\n"
              << "  -s <ip> <path>   Server to use, and its recording path (repeatable). Required.\n"
              << "  -b <kbps>        Target encoder bitrate. Default 10000.\n"
              << "  -i <sec>         Rollover interval; 0 = never roll over. Default 600.\n"
              << "  -n <name>        Instrument name, used in file naming. Default VISSS.\n"
              << "  -e <preset>      nvenc preset p1 (fastest) .. p7 (slowest/highest quality).\n"
              << "                   Default p1; p1-p5 measured clean at 485fps, p6/p7 overload.\n"
              << "  -r               Rotate recorded frame 90 degrees counterclockwise. Off by default.\n"
              << "  -m <20|30>       Motion-detection histogram bin-edge table. Default 20.\n"
              << "  -w               Disable recording-decision filtering (write every frame). Off by\n"
              << "                   default (filtering enabled).\n"
              << "  -l <ratio>       Refresh the live preview window every this-many frames. Default 70.\n"
              << "  --nopreview      Disable the live preview window entirely.\n"
              << "  -q <count>       Buffers pre-registered per output port (pipeline backpressure\n"
              << "                   slack). Default 8.\n"
              << "  -c <serial> <path>  Per-camera config file (plain text, \"FeatureName value\" per\n"
              << "                   line), applied to the matching camera after it's opened\n"
              << "                   (repeatable, one per camera). Optional - a camera with no\n"
              << "                   matching -c keeps its power-on-default parameters.\n"
              << "  --name <serial> <name>  Per-camera display name, used in that camera's own\n"
              << "                   overlay text/output filenames instead of -n (repeatable, one\n"
              << "                   per camera). Optional - a camera with no matching --name falls\n"
              << "                   back to -n. Needed whenever several cameras share one process\n"
              << "                   (e.g. a combined leader+follower deployment), since without it\n"
              << "                   every camera's output would show the same -n name.\n"
              << "  --site <name>    Site name shown in the status-bar overlay text. Default \"none\".\n"
              << "  --maxframes <n>  Debug: stop the run once ~n frames have elapsed (estimated from\n"
              << "                   camera FrameRate, not an exact count). Default: unlimited.\n"
              << "  -h, --help       Print this message and exit.\n\n"
              << "PTP sync is mandatory (not a flag): startup blocks until every camera reports PTP\n"
              << "Slave status, and aborts if any camera fails to lock within 30s.\n\n"
              << "See README.txt for full detail/rationale on each flag." << std::endl;
}

Params parseArgs(int argc, char* argv[])
{
    if (argc == 1)
    {
        PrintHelp(argv[0]);
        exit(0);
    }

    Params params{};

    for (int argIdx = 1; argIdx < argc; argIdx++)
    {
        std::string arg = argv[argIdx];
        // -h/--help is handled earlier, at the very top of main(), before any startup work -
        // this loop never sees it in practice.
        if (arg == "-s")
        {
            params.m_serverParams.push_back(ServerParams(argv[argIdx + 1], argv[argIdx + 2]));
            argIdx += 2;
        }
        else if (arg == "-b")
        {
            params.m_bitrateKbps = std::atoi(argv[argIdx + 1]);
            argIdx++;
        }
        else if (arg == "-i")
        {
            params.m_newFileIntervalSec = std::atoi(argv[argIdx + 1]);
            argIdx++;
        }
        else if (arg == "-n")
        {
            params.m_name = argv[argIdx + 1];
            argIdx++;
        }
        else if (arg == "-e")
        {
            params.m_preset = argv[argIdx + 1];
            argIdx++;
        }
        else if (arg == "-r")
        {
            params.m_rotate = true;
        }
        else if (arg == "-m")
        {
            params.m_minBrightChange = std::atoi(argv[argIdx + 1]);
            argIdx++;
        }
        else if (arg == "-w")
        {
            params.m_writeAllFrames = true;
        }
        else if (arg == "-l")
        {
            params.m_liveRatio = static_cast<uint32_t>(std::atoi(argv[argIdx + 1]));
            argIdx++;
        }
        else if (arg == "--nopreview")
        {
            params.m_noPreview = true;
        }
        else if (arg == "-q")
        {
            params.m_queueDepth = std::atoi(argv[argIdx + 1]);
            argIdx++;
        }
        else if (arg == "-c")
        {
            params.m_cameraConfigs.push_back(CameraConfigParams(argv[argIdx + 1], argv[argIdx + 2]));
            argIdx += 2;
        }
        else if (arg == "--name")
        {
            params.m_cameraNames.push_back(CameraNameParams(argv[argIdx + 1], argv[argIdx + 2]));
            argIdx += 2;
        }
        else if (arg == "--site")
        {
            params.m_site = argv[argIdx + 1];
            argIdx++;
        }
        else if (arg == "--maxframes")
        {
            params.m_maxFrames = static_cast<uint32_t>(std::atoi(argv[argIdx + 1]));
            argIdx++;
        }
    }

    return params;
}

// PTP sync is mandatory, not optional: downstream frame timestamps are only wall-clock-based when
// PTP is locked (see motion_detect's status-bar overlay), and matching up leader/follower frames
// depends on both cameras sharing a clock. This must not silently degrade to free-running
// unsynced capture, so failure to lock throws - main() lets that abort startup entirely, same
// "do not start capturing before PTP is confirmed synced" requirement as the old Teledyne
// pipeline (PROCESSING_SPEC_teeldyne.md §3.5), just against a different SDK's PTP status enum.
//
// Deliberately does NOT set TriggerMode/AcquisitionMode (unlike record_nvenc's own setPtp(),
// which additionally configures hardware-triggered simultaneous-start capture) - the old VISSS
// leader config runs untriggered (TriggerMode Off): PTP only disciplines each camera's clock so
// frames can be timestamp-matched afterward, cameras otherwise free-run independently.
void setPtp(std::vector<Camera> cams)
{
    for (auto& cam : cams)
    {
        cam.GetParameter<EnumCameraParam>("PtpMode").SetValue("TwoStep");
    }

    // Check if every camera has reached PTP slave status. PtpStatus has no "Master" state - per
    // the vendor's EVT_PTP example, cameras only ever sync as a slave to an external grandmaster
    // (here: the Mellanox NIC). 30 retries at 1s each, matching the old pipeline's own PTP-wait
    // timeout (§3.5) rather than record_nvenc's shorter 5s example default.
    for (auto& cam : cams)
    {
        EnumCameraParam ptpStatusParam = cam.GetParameter<EnumCameraParam>("PtpStatus");
        const std::string serial = std::to_string(cam.GetDiscoveryInfo().m_serialNumber);

        int numPtpCheck = 30;
        std::string status = ptpStatusParam.GetValue();
        while (status != "Slave")
        {
            std::cout << LogLine("INFO", serial, "waiting for PTP: " + status) << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            numPtpCheck--;
            status = ptpStatusParam.GetValue();
            if (numPtpCheck == 0)
            {
                throw std::runtime_error("Camera " + serial + " failed to reach PTP Slave status (currently: " +
                                         status + ")");
            }
        }
        std::cout << LogLine("INFO", serial, "PTP: " + status) << std::endl;
    }
}

// Reads each camera's temperature/PTP status and relays it to the matching RecordTask's metadata
// .txt header (§3.24) - RecordTask has no direct camera parameter access of its own. Called both
// eagerly once (right before recording starts, so the *first* segment's header already has real
// values instead of the "not yet read" placeholders) and periodically every 10 minutes (600s) from
// the main poll loop (so later segments - and the console - reflect any drift, e.g. rising
// temperature over a long recording session). This is also the only place temperature is ever
// logged/read at all - deliberately not on PrintStatusHeartbeat's ~1s cadence, see its comment.
void PollCameraStatus(const std::vector<Camera>& cams,
                      std::vector<std::pair<std::string, PluginTask>>& recordTasksById)
{
    for (const auto& cam : cams)
    {
        const std::string serial = std::to_string(cam.GetDiscoveryInfo().m_serialNumber);
        // SensTemp confirmed against this camera's actual feature list. PHYSNRMargin (which
        // would have been a substitute for the old pipeline's Teledyne-specific
        // transferQueueCurrentBlockCount/transferMaxBlockSize, see README.txt) turned out NOT to
        // exist on this camera despite being in Emergent's docs
        // ("Parameter PHYSNRMargin not found") - dropped rather than left failing every call,
        // which was also silencing the working SensTemp/PtpStatus fields since they shared one
        // try/catch. No network-link-health substitute currently available.
        try
        {
            const int32_t temperature = cam.GetParameter<Int32CameraParam>("SensTemp").GetValue();
            const std::string ptpStatus = cam.GetParameter<EnumCameraParam>("PtpStatus").GetValue();
            std::cout << LogLine("INFO", serial,
                                  "sensor temperature " + std::to_string(temperature) + " C, PTP " + ptpStatus)
                      << std::endl;

            for (auto& recordTaskEntry : recordTasksById)
            {
                if (recordTaskEntry.first == serial)
                {
                    recordTaskEntry.second.GetParameter<FloatTaskParam>(c_recordTemperatureParamName)
                        .SetValue(static_cast<float>(temperature));
                    recordTaskEntry.second.GetParameter<StringTaskParam>(c_recordPtpStatusParamName)
                        .SetValue(ptpStatus);
                    break;
                }
            }
        }
        catch (const std::exception& ex)
        {
            std::cout << LogLine("WARNING", serial, std::string("status query failed: ") + ex.what()) << std::endl;
        }
    }
}

// Periodic per-camera "STATUS" heartbeat (~1s, see the main poll loop), the client-side
// equivalent of the old Teledyne pipeline's once-per-second STATUS line
// (storage_worker_cv.h:743: "STATUS<id> | ts | Queue:N | ID:N | M:...") - downstream tooling (the
// Python launcher's per-camera status widget/Clean-button gating, see
// launch_visss_data_acquisition.py's update()) keys off seeing a line starting with "STATUS" to
// confirm the pipeline is actually alive, not just started.
//
// The old line's exact content (live queue length/histogram bin/move%) isn't reproducible here:
// those are computed entirely server-side inside MotionDetectTask/RecordTask's Process(), and
// eSDK Pro TaskParam values set from *inside* a plugin's own code never propagate back to a
// client GetParameter<T>().GetValue() call (confirmed by testing while building --maxframes - see
// recordtask.h's class doc for the full writeup; no DataOutput/DataInput port from those plugins
// to this client exists to carry it instead, unlike ShouldWrite between the two plugins
// themselves). So this reports what the client can actually observe directly: real camera PTP
// status plus an elapsed-time-based frame-count estimate (same technique as --maxframes), honestly
// labeled with "~" rather than presented as an exact live count. Temperature is deliberately NOT
// read/printed here (only PollCameraStatus does that, every 10 minutes, see its comment) - at ~1s
// cadence a per-heartbeat temperature read/line would be almost entirely redundant noise (sensor
// temperature drifts on the order of minutes, not seconds) for both the console and the
// TimedRotatingFileHandler-backed Python log file.
void PrintStatusHeartbeat(const std::vector<Camera>& cams,
                          const std::vector<std::pair<std::string, uint32_t>>& cameraFramerates,
                          const std::chrono::steady_clock::time_point& startTime)
{
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();

    for (const auto& cam : cams)
    {
        const std::string serial = std::to_string(cam.GetDiscoveryInfo().m_serialNumber);

        uint32_t framerate = 0;
        for (auto& entry : cameraFramerates)
        {
            if (entry.first == serial)
            {
                framerate = entry.second;
                break;
            }
        }
        const uint64_t estimatedFrames = (static_cast<uint64_t>(elapsedMs) * framerate) / 1000;

        try
        {
            const std::string ptpStatus = cam.GetParameter<EnumCameraParam>("PtpStatus").GetValue();
            std::cout << LogLine("STATUS", serial,
                                  "frames~" + std::to_string(estimatedFrames) + " | PTP " + ptpStatus)
                      << std::endl;
        }
        catch (const std::exception& ex)
        {
            std::cout << LogLine("WARNING", serial, std::string("status heartbeat failed: ") + ex.what())
                      << std::endl;
        }
    }
}

// Returns nullptr if this camera has no config file assigned - not every camera needs one.
const std::string* getCameraConfigPath(const std::string& serial, const Params& params)
{
    for (auto& cameraConfig : params.m_cameraConfigs)
    {
        if (cameraConfig.m_serial == serial)
        {
            return &cameraConfig.m_path;
        }
    }
    return nullptr;
}

// Per-camera display name (--name <serial> <name>) if this camera has one, else the shared -n
// value (params.m_name) - see CameraNameParams's comment for why a single shared name isn't
// enough once several cameras share one process.
std::string getCameraName(const std::string& serial, const Params& params)
{
    for (auto& cameraName : params.m_cameraNames)
    {
        if (cameraName.m_serial == serial)
        {
            return cameraName.m_name;
        }
    }
    return params.m_name;
}

// Client-side estimate of RecordTask's own rollover boundary and the exact final file path it
// will write there (mirrors motiondetecttask.cpp's identical "epoch seconds %
// NewFileIntervalSec == 0" check, itself derived from PTP-synced frame timestamps - see
// motiondetecttask.h's M: field comment) - computed independently client-side since eSDK Pro
// TaskParam values set inside a plugin's own code never propagate back to a client
// GetParameter<T>().GetValue() call (same one-way-sync limitation as the RecordTask console
// event relay, see recordtask.h's class doc), so the client can never be *told* by RecordTask
// itself when a segment actually rolls over or what it named the result. Uses wall clock
// (system_clock, not steady_clock) so the boundary aligns to the same epoch-second grid
// RecordTask/MotionDetectTask use (both derived from PTP-disciplined frame timestamps), not to
// this process's own start time - accurate as long as the host clock itself is disciplined
// (PTP/NTP), same assumption the rest of this pipeline already depends on. The path is built to
// match RecordTask::OpenSegment's naming convention byte-for-byte (recordtask.cpp) - hostname_
// name_deviceId/data/Y/m/d/hostname_name_deviceId_timestamp_0.mp4 - using the boundary second
// itself (bucket * newFileIntervalSec) as the timestamp, since that's exactly the frame
// timestamp at which RecordTask's own rollover condition first fires. lastBucket is caller-owned
// (persists across calls) so this only prints once per boundary crossing, not once per ~200ms
// poll tick.
void PrintNewFileNotice(const std::vector<Camera>& cams, const Params& params,
                        const std::vector<std::pair<std::string, std::string>>& cameraOutputRoots,
                        uint64_t startEpochS, uint64_t& lastBucket)
{
    const uint32_t newFileIntervalSec = params.m_newFileIntervalSec;
    if (newFileIntervalSec == 0)
    {
        // -i 0 means "never roll over" (see PrintHelp) - no periodic boundary to report.
        return;
    }
    const uint64_t nowEpochS = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    // Must match recordtask.cpp's identical c_minSecondsBeforeRollover - see RollSegmentIfNeeded's
    // comment for why. Duplicated rather than shared via a header (client and plugin are separate
    // CMake projects/binaries) - if you change one, change both.
    constexpr uint64_t c_minSecondsBeforeRollover = 10;
    if (nowEpochS < startEpochS + c_minSecondsBeforeRollover)
    {
        return;
    }
    const uint64_t bucket = nowEpochS / newFileIntervalSec;
    if (bucket == lastBucket)
    {
        return;
    }
    lastBucket = bucket;

    char hostnameBuf[256] = {0};
    gethostname(hostnameBuf, sizeof(hostnameBuf) - 1);
    const std::string hostname(hostnameBuf);

    const std::time_t segmentSeconds = static_cast<std::time_t>(bucket * newFileIntervalSec);
    std::tm tmBuf{};
    gmtime_r(&segmentSeconds, &tmBuf);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y%m%d-%H%M%S", &tmBuf);
    char dateDir[32];
    strftime(dateDir, sizeof(dateDir), "%Y/%m/%d", &tmBuf);

    for (const auto& cam : cams)
    {
        const std::string serial = std::to_string(cam.GetDiscoveryInfo().m_serialNumber);
        const std::string name = getCameraName(serial, params);

        std::string outputRoot;
        for (auto& entry : cameraOutputRoots)
        {
            if (entry.first == serial)
            {
                outputRoot = entry.second;
                break;
            }
        }

        const std::string baseName = hostname + "_" + name + "_" + serial + "_" + timeStr + "_0";
        const std::string finalMp4Path =
            outputRoot + "/" + hostname + "_" + name + "_" + serial + "/data/" + dateDir + "/" + baseName + ".mp4";
        std::cout << LogLine("INFO", serial, "new file (~): " + finalMp4Path) << std::endl;
    }
}

std::string getServerRecordPath(Server server, const Params& params)
{
    for (auto& serverParam : params.m_serverParams)
    {
        if (server.GetIp() == serverParam.m_ip)
        {
            return serverParam.m_recordPath;
        }
    }
    throw std::runtime_error("Failed to get record path for server " + server.GetIp());
}

// Tries one concrete CameraParam<T> type for 'name'. Returns false (never throws) if this type
// doesn't match - caller tries the next candidate type. Once a type match is confirmed, parsing
// 'value' and calling SetValue() are deliberately left OUTSIDE the catch, so a genuine bad value
// (malformed number, out-of-range) propagates as a real error distinguishable from "wrong type".
template <typename ParamT, typename Parser>
bool TrySetCameraParam(Camera& cam, const std::string& name, const std::string& value, Parser parse)
{
    ParamT param;
    try
    {
        param = cam.GetParameter<ParamT>(name);
    }
    catch (const ESdkProException&)
    {
        return false;
    }
    param.SetValue(parse(value));
    return true;
}

bool TrySetCameraCommandParam(Camera& cam, const std::string& name)
{
    CommandCameraParam param;
    try
    {
        param = cam.GetParameter<CommandCameraParam>(name);
    }
    catch (const ESdkProException&)
    {
        return false;
    }
    param.Execute(); // command params take no value; the file's 2nd token is ignored, matching
                      // GenApi ICommand::FromString()'s own behavior on the old pipeline
    return true;
}

// Sets one GenICam feature by name+string value, probing each known CameraParamType in turn -
// eSDK Pro has no type-erased setter and no way to query a node's type without already picking
// one (confirmed against the SDK headers, not assumed), but GetParameter<T>(name) itself
// validates name+type and throws if wrong (confirmed by this file's own PHYSNRMargin-not-found
// handling elsewhere), and every GenICam node has exactly one true CameraParamType - so trying
// each candidate T until one's GetParameter<T>() succeeds reconstructs generic dispatch using
// only the public API. This is what lets the config file gain new parameter names later with
// zero C++ changes, matching the old pipeline's real flexibility.
void SetCameraParamGeneric(Camera& cam, const std::string& name, const std::string& value)
{
    if (TrySetCameraParam<UInt32CameraParam>(cam, name, value,
                                              [](const std::string& v) { return static_cast<uint32_t>(std::stoul(v)); }))
    {
        return;
    }
    if (TrySetCameraParam<Int32CameraParam>(cam, name, value,
                                             [](const std::string& v) { return static_cast<int32_t>(std::stol(v)); }))
    {
        return;
    }
    if (TrySetCameraParam<FloatCameraParam>(cam, name, value, [](const std::string& v) { return std::stof(v); }))
    {
        return;
    }
    if (TrySetCameraParam<BoolCameraParam>(
            cam, name, value, [](const std::string& v) { return v == "1" || v == "true" || v == "True"; }))
    {
        return;
    }
    if (TrySetCameraParam<EnumCameraParam>(cam, name, value, [](const std::string& v) { return v; }))
    {
        return;
    }
    if (TrySetCameraParam<StringCameraParam>(cam, name, value, [](const std::string& v) { return v; }))
    {
        return;
    }
    if (TrySetCameraParam<RegisterCameraParam>(cam, name, value, [](const std::string& v) { return v; }))
    {
        return;
    }
    if (TrySetCameraCommandParam(cam, name))
    {
        return;
    }

    throw std::runtime_error("no matching camera parameter of any known type");
}

// Applies a plain-text "FeatureName value" camera-parameter config file to one camera, one
// GenICam feature per line - same format as the old Teledyne pipeline's config file
// (data-acquisition/src/visss-data-acquisition.cpp's fscanf("%s %s") loop), just parsed via
// operator>> instead. Lines are applied strictly in file order, so a future config can put a
// prerequisite toggle on an earlier line before the feature it gates, with no special-casing
// here. Matches the old pipeline's real error policy (confirmed by reading its
// error_count/FATAL-ERROR logic, not assumed): process the WHOLE file, log every failure as it's
// found (so one run's log shows every problem, not just the first), then throw if anything
// failed - caught by main()'s existing outer catch, same fatal-startup path as setPtp()'s
// failure. A missing/unreadable file is immediately fatal (deployment/typo bug, no reasonable
// "continue").
void ApplyCameraConfigFile(Camera& cam, const std::string& deviceId, const std::string& path)
{
    std::ifstream configStream(path);
    if (!configStream)
    {
        throw std::runtime_error("Failed to open camera config file: " + path);
    }

    int appliedCount = 0;
    int errorCount = 0;
    std::string featureName;
    std::string valueStr;
    while (configStream >> featureName >> valueStr)
    {
        try
        {
            SetCameraParamGeneric(cam, featureName, valueStr);
            std::cout << LogLine("INFO", deviceId, "camera config: " + featureName + " = " + valueStr)
                      << std::endl;
            appliedCount++;
        }
        catch (const std::exception& ex)
        {
            std::cout << LogLine("ERROR", deviceId,
                                  "camera config: failed to set " + featureName + " = " + valueStr + " (" +
                                      ex.what() + ")")
                      << std::endl;
            errorCount++;
        }
    }

    if (errorCount > 0)
    {
        throw std::runtime_error(std::to_string(errorCount) + " camera config parameter(s) failed to apply from " +
                                  path + " (" + std::to_string(appliedCount) +
                                  " applied successfully) - see ERROR lines above");
    }
    std::cout << LogLine("INFO", deviceId,
                          std::to_string(appliedCount) + " camera config parameter(s) applied from " + path)
              << std::endl;
}

int main(int argc, char* argv[])
{
    // Checked before any startup work (banner print, SDK System::Create(), etc.) so -h/--help
    // gives a clean, immediate help print and nothing else - parseArgs() below also checks this,
    // but only after the banner/command-line echo have already printed.
    for (int argIdx = 1; argIdx < argc; argIdx++)
    {
        const std::string arg = argv[argIdx];
        if (arg == "-h" || arg == "--help")
        {
            PrintHelp(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    System system = System::Create();
    // Default is Warning (per eSdkPro/core.h) - Info is needed for eSDK Pro's own client-side
    // Info-level messages to show. Note this has no effect on Plugin::TaskWorker::LogMessage()
    // calls made from MotionDetectTask/RecordTask: those run inside the separate eCaptureProServer
    // process (systemd unit has StandardOutput=null, so that process's own log output goes
    // nowhere visible either way) - there is no relay from server-side LogMessage() to the client,
    // confirmed by testing. RecordTask's segment-lifecycle events are instead relayed via the
    // LastEvent task parameter and printed from here - see the polling loop below.
    SetLogLevel(LogLevel::Info);
    int retCode = EXIT_SUCCESS;

    try
    {
        std::cout << LogLine("INFO", "", "VISSS data acquisition (EVT)") << std::endl;
        std::string commandLine;
        for (int i = 0; i < argc; i++)
        {
            if (i > 0)
            {
                commandLine += " ";
            }
            commandLine += argv[i];
        }
        std::cout << LogLine("BASH", "", commandLine) << std::endl;

        // Parse arguments
        Params params = parseArgs(argc, argv);
        std::cout << "Servers:" << std::endl;
        for (auto& serverParam : params.m_serverParams)
        {
            std::cout << "\t" << "IP: " << serverParam.m_ip << " Record Path: " << serverParam.m_recordPath
                      << std::endl;
        }
        std::cout << "Site: " << params.m_site << std::endl;
        if (params.m_maxFrames > 0)
        {
            std::cout << "Max frames: " << params.m_maxFrames << " (estimated from elapsed time, not an exact count)"
                      << std::endl;
        }
        else
        {
            std::cout << "Max frames: unlimited (run until interrupted, Ctrl+C)" << std::endl;
        }
        std::cout << "Bitrate: " << params.m_bitrateKbps << " Kbps" << std::endl;
        std::cout << "New file interval: " << params.m_newFileIntervalSec << " sec" << std::endl;
        std::cout << "Name: " << params.m_name << (params.m_cameraNames.empty() ? "" : " (default/fallback)")
                  << std::endl;
        for (auto& cameraName : params.m_cameraNames)
        {
            std::cout << "\t" << "Serial: " << cameraName.m_serial << " Name: " << cameraName.m_name << std::endl;
        }
        std::cout << "nvenc preset: " << params.m_preset << std::endl;
        std::cout << "Rotate: " << (params.m_rotate ? "on (90 CCW)" : "off") << std::endl;
        std::cout << "MinBrightChange: " << params.m_minBrightChange << std::endl;
        std::cout << "WriteAllFrames: " << (params.m_writeAllFrames ? "on (filtering disabled)" : "off")
                  << std::endl;
        std::cout << "Preview: " << (params.m_noPreview ? "off" : ("on, every " +
                                                                    std::to_string(params.m_liveRatio) + " frames"))
                  << std::endl;
        std::cout << "QueueDepth: " << params.m_queueDepth << std::endl;
        if (params.m_cameraConfigs.empty())
        {
            std::cout << "Camera config files: none (power-on defaults)" << std::endl;
        }
        else
        {
            std::cout << "Camera config files:" << std::endl;
            for (auto& cameraConfig : params.m_cameraConfigs)
            {
                std::cout << "\t" << "Serial: " << cameraConfig.m_serial << " Path: " << cameraConfig.m_path
                          << std::endl;
            }
        }
        std::cout << "============" << std::endl;

        // Connect to servers
        for (auto& serverParam : params.m_serverParams)
        {
            system.ConnectServer(serverParam.m_ip);
        }

        // Open all cameras using the same settings
        CameraOpenConfig camOpenConfig{};
        camOpenConfig.m_gpuDeviceId = c_gpuId;
        camOpenConfig.m_gpuDirectEnabled = c_useGpuDirect;

        // Discover and open cameras
        for (auto& server : system.GetServers())
        {
            std::cout << LogLine("INFO", "", "Server " + server.GetIp()) << std::endl;
            std::vector<CameraDiscoveryInfo> allDiscoveredCamInfo = server.DiscoverCameras();
            std::cout << LogLine("INFO", "", std::to_string(allDiscoveredCamInfo.size()) + " camera(s) detected")
                      << std::endl;
            for (auto& discoveredCamInfo : allDiscoveredCamInfo)
            {
                const std::string serial = std::to_string(discoveredCamInfo.m_serialNumber);
                std::cout << LogLine("INFO", serial, "Adding camera [" + discoveredCamInfo.m_modelName + "]")
                          << std::endl;

                Camera cam = server.AddCamera(discoveredCamInfo);
                cam.Open(camOpenConfig);

                const std::string* cameraConfigPath = getCameraConfigPath(serial, params);
                if (cameraConfigPath != nullptr)
                {
                    ApplyCameraConfigFile(cam, serial, *cameraConfigPath);
                }
                else
                {
                    // Not just cosmetic: silently running a camera at power-on defaults (no
                    // exposure/framerate/etc override) is exactly the kind of gap worth surfacing
                    // loudly rather than discovering later in the recorded data. Matches the
                    // Python launcher's own warning for the same condition (see
                    // launch_visss_data_acquisition.py's EmergentInstrument) - this one fires
                    // even when the binary is run directly, bypassing the Python wrapper.
                    std::cout << LogLine("WARNING", serial,
                                          "no camera config file (-c) given for this camera - "
                                          "running at power-on-default settings")
                              << std::endl;
                }
            }
        }

        Pipeline pipeline = system.GetPipeline();

        // PTP sync is mandatory (see setPtp()'s comment) - waits here for lock, then
        // SetPtpSyncMode(true) additionally makes Pipeline::Start() itself refuse to run if any
        // camera loses sync before then.
        std::cout << LogLine("INFO", "", "Waiting for PTP sync...") << std::endl;
        setPtp(system.GetCameras());
        pipeline.SetPtpSyncMode(true);
        std::cout << LogLine("INFO", "", "PTP synced.") << std::endl;

        // Track each camera's RecordTask handle + the last-seen value of each event param (see
        // c_recordEventParamNames), so the polling loop below can detect and relay new
        // segment-lifecycle events without needing a growing/queued log buffer.
        std::vector<std::pair<std::string, PluginTask>> recordTasksById;
        std::vector<std::vector<std::string>> recordTasksLastEvent;
        // Used for --maxframes (see the main poll loop): each camera's actual FrameRate, so
        // elapsed time can be converted to an estimated frame count. There is no live frame-count
        // readout available to the client (see Params::m_maxFrames's comment).
        std::vector<std::pair<std::string, uint32_t>> cameraFramerates;
        // Each camera's server recording path (OutputRoot) - used by PrintNewFileNotice to
        // reconstruct RecordTask's own final file path client-side (see its comment for why).
        std::vector<std::pair<std::string, std::string>> cameraOutputRoots;

        // Initialize pipeline
        for (auto& server : system.GetServers())
        {
            std::string recordPath = getServerRecordPath(server, params);
            std::filesystem::create_directories(recordPath);

            for (auto& cam : server.GetCameras())
            {
                const std::string deviceId = std::to_string(cam.GetDiscoveryInfo().m_serialNumber);

                // Create camera task to use camera within pipeline
                CameraTask camTask = pipeline.CreateCameraTask(cam);

                UInt32CameraParam widthParam = cam.GetParameter<UInt32CameraParam>("Width");
                UInt32CameraParam heightParam = cam.GetParameter<UInt32CameraParam>("Height");
                UInt32CameraParam framerateParam = cam.GetParameter<UInt32CameraParam>("FrameRate");

                // Create the motion-detect plugin task (currently: reserves a black border region
                // at the top of each frame, plus optional rotation; motion detection itself is not
                // implemented yet - see motion_detect/README.txt).
                PluginTask motionDetectTask = pipeline.CreatePluginTask(server, c_motionDetectTaskName);
                motionDetectTask.GetParameter<StringTaskParam>(c_motionDetectSiteParamName).SetValue(params.m_site);
                motionDetectTask.GetParameter<BoolTaskParam>(c_motionDetectRotateParamName)
                    .SetValue(params.m_rotate);
                motionDetectTask.GetParameter<Int32TaskParam>(c_motionDetectMinBrightChangeParamName)
                    .SetValue(params.m_minBrightChange);
                motionDetectTask.GetParameter<BoolTaskParam>(c_motionDetectWriteAllFramesParamName)
                    .SetValue(params.m_writeAllFrames);
                motionDetectTask.GetParameter<Int32TaskParam>(c_motionDetectFramerateParamName)
                    .SetValue(static_cast<int32_t>(framerateParam.GetValue()));
                motionDetectTask.GetParameter<Int32TaskParam>(c_motionDetectLiveRatioParamName)
                    .SetValue(static_cast<int32_t>(params.m_liveRatio));
                motionDetectTask.GetParameter<BoolTaskParam>(c_motionDetectNoPreviewParamName)
                    .SetValue(params.m_noPreview);
                motionDetectTask.GetParameter<Int32TaskParam>(c_motionDetectQueueDepthParamName)
                    .SetValue(params.m_queueDepth);
                // Was missing entirely - the overlay's "name" field silently showed
                // MotionDetectTask's own default ("VISSS") regardless of -n, never the real value.
                // getCameraName() falls back to the shared -n value if this camera has no
                // --name <serial> <name> override (see CameraNameParams's comment) - this is what
                // makes each physical camera's own overlay text/output filenames show its own
                // name rather than every camera sharing whichever name -n happens to be.
                const std::string cameraName = getCameraName(deviceId, params);
                motionDetectTask.GetParameter<StringTaskParam>(c_motionDetectNameParamName).SetValue(cameraName);
                motionDetectTask.GetParameter<Int32TaskParam>(c_motionDetectNewFileIntervalSecParamName)
                    .SetValue(static_cast<int32_t>(params.m_newFileIntervalSec));

                // Create the record plugin task, replacing NvencTask - see ../record/README.txt
                // for why. Width/Height must match what MotionDetectTask actually outputs
                // (camera's own frame size plus its added border, width/height swapped if rotation
                // is on - see motiondetecttask.h) - RecordTask sets up its NVENC session eagerly in
                // Init() using these, rather than lazily on the first frame, since NVENC session
                // creation is too slow to do on the per-frame hot path at high frame rates.
                // RecordTask builds the full VISSS folder/naming convention and handles rollover
                // itself (see recordtask.h) - OutputRoot is just the base directory, not a full
                // file path.
                const uint32_t contentWidth = params.m_rotate ? heightParam.GetValue() : widthParam.GetValue();
                const uint32_t contentHeight = params.m_rotate ? widthParam.GetValue() : heightParam.GetValue();

                PluginTask recordTask = pipeline.CreatePluginTask(server, c_recordTaskName);
                recordTask.GetParameter<StringTaskParam>(c_recordOutputRootParamName).SetValue(recordPath);
                recordTask.GetParameter<StringTaskParam>(c_recordNameParamName).SetValue(cameraName);
                recordTask.GetParameter<StringTaskParam>(c_recordDeviceIdParamName).SetValue(deviceId);
                recordTask.GetParameter<Int32TaskParam>(c_recordWidthParamName)
                    .SetValue(static_cast<int32_t>(contentWidth));
                recordTask.GetParameter<Int32TaskParam>(c_recordHeightParamName)
                    .SetValue(static_cast<int32_t>(contentHeight + c_motionDetectBorderHeight));
                recordTask.GetParameter<Int32TaskParam>(c_recordFramerateParamName)
                    .SetValue(static_cast<int32_t>(framerateParam.GetValue()));
                recordTask.GetParameter<Int32TaskParam>(c_recordBitrateKbpsParamName)
                    .SetValue(static_cast<int32_t>(params.m_bitrateKbps));
                recordTask.GetParameter<Int32TaskParam>(c_recordNewFileIntervalSecParamName)
                    .SetValue(static_cast<int32_t>(params.m_newFileIntervalSec));
                recordTask.GetParameter<StringTaskParam>(c_recordPresetParamName).SetValue(params.m_preset);
                recordTask.GetParameter<Int32TaskParam>(c_recordMinBrightChangeParamName)
                    .SetValue(params.m_minBrightChange);
                const std::string* deviceCameraConfigPath = getCameraConfigPath(deviceId, params);
                recordTask.GetParameter<StringTaskParam>(c_recordCameraConfigNameParamName)
                    .SetValue(deviceCameraConfigPath == nullptr
                                  ? "none"
                                  : std::filesystem::path(*deviceCameraConfigPath).filename().string());

                // Connect the camera output to the motion-detect plugin, then the plugin to Record
                pipeline.ConnectTasks(camTask.GetOutput(), motionDetectTask.GetInput(c_motionDetectInputName));
                pipeline.ConnectTasks(motionDetectTask.GetOutput(c_motionDetectOutputName),
                                       recordTask.GetInput(c_recordInputName));
                pipeline.ConnectTasks(motionDetectTask.GetOutput(c_motionDetectShouldWriteOutputName),
                                       recordTask.GetInput(c_recordShouldWriteInputName));

                // Live preview (§3.25), a real cv::imshow window - see OnPreviewFrame's comment.
                // The callback runs in this client process (not inside eCaptureProServer), so it's
                // set up here rather than in MotionDetectTask/RecordTask.
                if (!params.m_noPreview)
                {
                    PreviewFrameFunc previewCallback = [deviceId](eSdkPro::Frame frame)
                    { OnPreviewFrame(deviceId, frame); };
                    ImageDisplayTask imageDisplayTask = pipeline.CreateImageDisplayTask(server, previewCallback);
                    // Left at its default (Host-platform expectation, no SetGpuDeviceId call) -
                    // MotionDetectTask downloads the downscaled preview to host before pushing it
                    // (see motiondetecttask.cpp), matching this default rather than fighting it:
                    // an earlier Cuda-platform attempt required SetGpuDeviceId() to even connect,
                    // but delivered visibly corrupted/striped frames to this callback (a pitch
                    // handling issue in that path, confirmed by testing, not something this
                    // client controls).
                    pipeline.ConnectTasks(motionDetectTask.GetOutput(c_motionDetectPreviewOutputName),
                                           imageDisplayTask.GetInput());
                }

                recordTasksById.push_back(std::make_pair(deviceId, recordTask));
                recordTasksLastEvent.push_back(std::vector<std::string>(c_recordEventParamNames.size(), ""));
                cameraFramerates.push_back(std::make_pair(deviceId, framerateParam.GetValue()));
                cameraOutputRoots.push_back(std::make_pair(deviceId, recordPath));
            }
        }

        std::cout << LogLine("INFO", "", "Starting recording") << std::endl;
        pipeline.Start();

        if (params.m_maxFrames > 0)
        {
            std::cout << LogLine("INFO", "",
                                  "Recording until ~" + std::to_string(params.m_maxFrames) + " frames...")
                      << std::endl;
        }
        else
        {
            std::cout << LogLine("INFO", "", "Recording until interrupted (Ctrl+C)...") << std::endl;
        }

        // Poll instead of a single sleep_for(): needs to notice g_shouldStop promptly (short poll
        // interval), relay RecordTask's LastEvent task param changes (see the comment on
        // SetLogLevel() above for why that relay exists), and print periodic camera status
        // (matches the old pipeline's own periodic status refresh, PROCESSING_SPEC_teeldyne.md
        // §3.13, minus the Teledyne-specific transport counters that have no eSDK Pro equivalent)
        // - while still supporting an optional frame-count limit for testing (--maxframes).
        const auto startTime = std::chrono::steady_clock::now();
        auto lastStatusTime = startTime;
        auto lastHeartbeatTime = startTime;
        const std::vector<Camera> cams = system.GetCameras();

        // Eager first read (see PollCameraStatus's comment) so the first segment's metadata
        // header already has real temperature/PTP values instead of "not yet read" placeholders.
        PollCameraStatus(cams, recordTasksById);

        // Software-start epoch second, used by PrintNewFileNotice to suppress a boundary crossing
        // that RecordTask itself would also suppress (recordtask.cpp's
        // c_minSecondsBeforeRollover) - kept in sync so this client-side prediction doesn't print
        // a "new file" notice for a rollover that never actually happens.
        const uint64_t startEpochS = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());

        // Initialized to the *current* boundary (not 0) so PrintNewFileNotice doesn't fire
        // immediately for the segment that's already being opened right now - only for
        // subsequent rollovers.
        uint64_t lastNewFileBucket =
            (params.m_newFileIntervalSec == 0) ? 0 : startEpochS / params.m_newFileIntervalSec;

        while (!g_shouldStop.load())
        {
            const auto now = std::chrono::steady_clock::now();
            if (params.m_maxFrames > 0)
            {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                bool reachedMaxFrames = false;
                for (auto& entry : cameraFramerates)
                {
                    const uint64_t estimatedFrames =
                        (static_cast<uint64_t>(elapsedMs) * entry.second) / 1000;
                    if (estimatedFrames >= params.m_maxFrames)
                    {
                        std::cout << LogLine("INFO", entry.first,
                                              "reached ~max frames (~" + std::to_string(estimatedFrames) +
                                                  " estimated), stopping")
                                  << std::endl;
                        reachedMaxFrames = true;
                        break;
                    }
                }
                if (reachedMaxFrames)
                {
                    break;
                }
            }

            for (size_t i = 0; i < recordTasksById.size(); i++)
            {
                for (size_t p = 0; p < c_recordEventParamNames.size(); p++)
                {
                    const std::string event =
                        recordTasksById[i].second.GetParameter<StringTaskParam>(c_recordEventParamNames[p]).GetValue();
                    if (!event.empty() && event != recordTasksLastEvent[i][p])
                    {
                        std::cout << LogLine("INFO", recordTasksById[i].first, event) << std::endl;
                        recordTasksLastEvent[i][p] = event;
                    }
                }
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStatusTime).count() >= 600)
            {
                PollCameraStatus(cams, recordTasksById);
                lastStatusTime = now;
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeatTime).count() >= 1)
            {
                PrintStatusHeartbeat(cams, cameraFramerates, startTime);
                lastHeartbeatTime = now;
            }

            PrintNewFileNotice(cams, params, cameraOutputRoots, startEpochS, lastNewFileBucket);

            // cv::imshow/cv::waitKey must run consistently from one thread (GTK-backed highgui
            // isn't safe to call from arbitrary threads) - this loop is that one thread; the
            // ImageDisplayTask callbacks (OnPreviewFrame, background threads, one per camera)
            // only ever touch the mutex-guarded g_previewFrames map, never call into highgui
            // directly. waitKey(1) is required, not optional - it's what actually pumps the GUI
            // event loop/repaints the window(s); without it the window never updates or even
            // appears responsive.
            if (!params.m_noPreview)
            {
                std::lock_guard<std::mutex> lock(g_previewMutex);
                for (const auto& entry : g_previewFrames)
                {
                    cv::imshow("VISSS " + entry.first, entry.second);
                }
                cv::waitKey(1);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::cout << LogLine("INFO", "", "Stopping recording") << std::endl;
        pipeline.Stop();
        std::cout << LogLine("INFO", "", "Done.") << std::endl;
    }
    catch (const std::exception& ex)
    {
        std::cout << LogLine("ERROR", "", std::string("Runtime error exception: ") + ex.what()) << std::endl;
        retCode = EXIT_FAILURE;
    }

    cv::destroyAllWindows();

    // Release all managed system resources
    system.Destroy();

    return retCode;
}
