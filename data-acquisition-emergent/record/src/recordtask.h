#pragma once

#include <eSdkPro/plugin/taskworker.h>

#include <fstream>
#include <stdint.h>
#include <string>

extern "C"
{
    struct AVCodecContext;
    struct AVFormatContext;
    struct AVBufferRef;
    struct AVStream;
    struct AVFrame;
}

namespace Record
{
const std::string c_taskName = "Record";
const std::string c_inputName = "InFrame";
// Companion data port from MotionDetectTask carrying the §3.21 recording decision plus the §3.18
// histogram counts (see ShouldWritePortPayload below) for every frame - see motiondetecttask.h's
// class doc comment for why frame filtering is done this way instead of MotionDetectTask simply
// not pushing filtered frames.
const std::string c_shouldWriteInputName = "ShouldWrite";
// Selects the §3.18 bin-edge table for the metadata header's column-name line (must match
// MotionDetectTask's MinBrightChange, since only it actually computes the histogram - this task
// just needs to know which edges to print).
const std::string c_minBrightChangeParamName = "MinBrightChange";
// Updated periodically by the client (matches the old pipeline's cross-thread-shared
// cameraTemperature/ptp_status globals, PROCESSING_SPEC_teeldyne.md §5) for the metadata header's
// "Camera Temperature"/"PTP Status" lines - this task has no direct camera parameter access.
const std::string c_temperatureParamName = "Temperature";
const std::string c_ptpStatusParamName = "PtpStatus";
// Basename of the client's -c camera-parameter config file (or "none" if not given), set once at
// setup time - for the metadata header's "Camera configuration" line (§3.24). This task has no
// visibility into the client's CLI args otherwise.
const std::string c_cameraConfigNameParamName = "CameraConfigName";
// FloatTaskParam's default before the client's first 30s status poll. Not NaN: the pipeline's
// internal JSON serialization of task param defaults rejects NaN ("type must be number, but is
// null", confirmed by testing) - an implausible finite value works as a sentinel instead. Printed
// as "nan" in the metadata header (matching the old pipeline's own "not yet read" convention) when
// seen at header-write time.
constexpr float c_temperatureNotYetReadSentinel = -9999.0f;

// Task parameters, set by the client after CreatePluginTask() (see main.cpp) - equivalent to
// NvencTask::InitParams, since a PluginTask has no InitParams struct of its own. Width/Height
// must be known upfront (rather than lazily read from the first frame) so the encoder can be set
// up once in Init() - see the class doc comment for why that matters.
const std::string c_outputRootParamName = "OutputRoot";
const std::string c_nameParamName = "Name";
const std::string c_deviceIdParamName = "DeviceId";
const std::string c_widthParamName = "Width";
const std::string c_heightParamName = "Height";
const std::string c_framerateParamName = "Framerate";
const std::string c_bitrateKbpsParamName = "BitrateKbps";
const std::string c_newFileIntervalSecParamName = "NewFileIntervalSec";
// nvenc preset (p1 fastest/lowest-quality .. p7 slowest/highest-quality at a fixed bitrate).
// Measured against real hardware at 485fps, both cameras, -w (unfiltered) for sustained load,
// 15s runs: p1-p5 clean (0 dropped, 0 missed, verified via both cameras' server-side per-session
// counters and output frame-ID continuity); p6/p7 overload - "Missed Save" grows linearly for the
// whole run (not just an initial burst), confirming a sustained NVENC throughput shortfall at
// this quality level, not a transient one. Raising QueueDepth (see main.cpp's -q /
// motiondetecttask.h) does NOT rescue p6/p7 - confirmed by testing with QueueDepth 1 vs. 4 at
// both p4 (already clean either way) and p7 (overloads at the same rate either way): a bigger
// buffer pool only absorbs bursty/transient slowdowns, not a sustained shortfall. These numbers
// are current-hardware/current-firmware specific (this session's GPU: RTX PRO 4000 Blackwell) and
// obsolete the original porting session's more conservative p1-safe/p4-drops finding - re-measure
// if the GPU, driver, or camera config changes. Exposed as a parameter (and a client CLI flag,
// see main.cpp's -e) specifically so this can keep being tuned without a rebuild.
const std::string c_presetParamName = "Preset";
// Read-only (client reads, never sets): lifecycle event messages, polled by the client (see the
// class doc comment on why this exists instead of LogMessage()) and printed with its own
// "LEVEL-id | timestamp | message" formatting. Four separate params, not one: at a rollover,
// CloseSegment/OpenSegment/SaveSnapshot all fire within the same Process() call, faster than the
// client's poll interval - a single shared string would have later events silently overwrite
// earlier ones before the client ever sees them.
const std::string c_lastSessionEventParamName = "LastSessionEvent";
const std::string c_lastSegmentClosedParamName = "LastSegmentClosed";
const std::string c_lastSegmentStartedParamName = "LastSegmentStarted";
const std::string c_lastSnapshotParamName = "LastSnapshot";
} // namespace Record

// Wire format for the ShouldWrite companion data port (see Record::c_shouldWriteInputName above).
// Kept byte-for-byte identical to MotionDetect::ShouldWritePortPayload in motiondetecttask.h - see
// that copy's comment for why this is duplicated rather than shared. If you change this, change
// motiondetecttask.h's copy too.
#pragma pack(push, 1)
struct ShouldWritePortPayload
{
    uint8_t shouldWrite;    // §3.21 recording decision: 0 = skip, 1 = write
    uint32_t queueLength;   // §3.20/§3.24 "Q:"/Queue Length - see MotionDetect's QueueDepth comment
    uint32_t histCounts[7]; // §3.18 step 3 cumulative histogram counts (>= edge[i])
};
#pragma pack(pop)

/**
 * Encodes GPU-resident frames via FFmpeg's hevc_nvenc and muxes them directly to a file,
 * replacing eSDK Pro's built-in NvencTask.
 *
 * Why not NvencTask: its native segmented-recording feature (eCapturePro 1.6.1) rotates the video
 * file but not the accompanying recordingTimestamps.txt/recordingMetadata.json, which are
 * constructed once per whole recording session (confirmed via symbols in libeSdkPro.so -
 * NvencTaskWorker has a single constructor call, not one per segment). VISSS needs the per-frame
 * timestamp file to be exactly synced with the video, frame for frame - encoding ourselves lets
 * us write both together in the same Process() call, sync-by-construction: a frame that never
 * reaches Process() (dropped upstream) is simply absent from both outputs, never mismatched.
 *
 * File naming/layout matches the old pipeline (PROCESSING_SPEC_teeldyne.md §3.22), with two
 * deliberate differences: the container is .mp4 (not .mkv - matches what hevc_nvenc/libavformat
 * actually produces) and the thread-id suffix is hardcoded to "_0" (round-robin thread-splitting
 * isn't ported - PROCESSING_SPEC_teeldyne.md §3.15 - kept only for filename-shape compatibility
 * with VISSSlib's existing parser):
 *   staging: {OutputRoot}/tmp/{hostname}_{Name}_{DeviceId}_{YYYYMMDD-HHMMSS}_0.{mp4,txt}
 *   final:   {OutputRoot}/{hostname}_{Name}_{DeviceId}/data/{YYYY}/{MM}/{DD}/<same base>.{mp4,txt,jpg}
 *   latest:  {OutputRoot}/{Name}_{DeviceId}_latest_0.{mp4,txt,jpg} -> final path
 *     (DeviceId included here too, not just in staging/final - Name alone is shared across every
 *     camera one client process manages, confirmed by testing: a real 2-camera-per-server
 *     deployment produced only one set of _latest files without it, the two cameras' RecordTask
 *     instances racing to overwrite the same symlink. Fixed 2026-08-11 - see
 *     launch_visss_data_acquisition.py's EmergentInstrument, which must construct the matching
 *     path for its wiper brightness check.)
 * Rollover happens every NewFileIntervalSec seconds (0 = never), using the exact same trigger
 * logic as the old pipeline's do_housekeeping (§3.13): fires once per interval boundary, debounced
 * so it can't re-fire on every frame within the same boundary second.
 *
 * Metadata .txt: §3.24 format, version "e.1" (this port's own version series - deliberately not
 * a Teledyne "0.x" number, since this is a different/incompatible rewrite against a different
 * SDK, not a continuation of that version history), column layout matching a real Teledyne
 * production sample (visss11gb_visss_leader_..._0.txt) as closely as this port's actual data
 * allows: a "# key: value" header (file format version, git tag/branch, camera reset time, us
 * since epoche, serial, config basename, hostname, temperature, PTP status) followed by
 * "# Capture time, Record time, Frame id, Queue Length, <bin edges>" and one CSV row per frame
 * actually written. Two Teledyne-only transport diagnostics
 * (transferQueueCurrentBlockCount/transferMaxBlockSize) have no eSDK Pro equivalent (confirmed,
 * see main.cpp's PHYSNRMargin note) and are dropped from the header entirely - not applicable to
 * this hardware/SDK, not worth a permanent "-99" placeholder line. "Camera configuration" is the
 * client's -c config file's basename (see CameraConfigName), or "none" if -c wasn't given.
 * "PTP Status" defaults to "Slave", not "unknown" - by construction, any frame RecordTask ever
 * sees was captured after main.cpp's mandatory PTP-lock gate (setPtp() + SetPtpSyncMode(true))
 * already succeeded, so "unknown" was never actually possible, just an artifact of waiting for
 * the first status poll; the client's periodic 30s poll still keeps this live for later segments
 * in case sync degrades mid-session. "Queue Length" is MotionDetectTask's queueLength estimate
 * (see its QueueDepth comment) - not a literal eSDK Pro buffer-occupancy count (no such API
 * exists, mapping doc gap list #3), but a derived frame-equivalent backlog estimate serving the
 * same purpose.
 *
 * First-frame .jpg snapshot: written straight to its final path (no staging - matches the old
 * pipeline's own cv::imwrite(filename_final_ + ".jpg", ...), storage_worker_cv.h:676) the moment
 * a new segment's first frame arrives. Downloaded to host and encoded via libavcodec's mjpeg
 * encoder (already linked, no new dependency) rather than kept on GPU: unlike the per-frame video
 * path, this runs once per rollover (every NewFileIntervalSec, default 600s), so a small one-off
 * CPU round-trip has no bearing on the real-time pipeline's GPU-resident requirement.
 *
 * Pixel format: input frames are GVSP_PIX_MONO8 (single channel), but hevc_nvenc only accepts
 * YUV-family formats (no mono8) - Process() converts to NV12 on the GPU with zero CUDA-kernel
 * code needed: the Y plane is a straight cudaMemcpy2D of the mono data, and the UV plane is
 * cudaMemset2D'd to a constant 128 (neutral chroma, i.e. "no color"), matching the old pipeline's
 * gray-only assumption (PROCESSING_SPEC_teeldyne.md §3.14).
 *
 * Logging: LogMessage() calls end up nowhere visible - this plugin runs inside eCaptureProServer,
 * whose systemd unit sets StandardOutput=null/StandardError=null, and there is no relay from
 * server-side LogMessage() to a connected client (confirmed by testing: raising the client's own
 * SetLogLevel() has no effect on plugin-side messages). Segment-lifecycle events are published via
 * the LastEvent task parameter for the client to poll and print - but as of this writing that
 * poll never actually sees anything (confirmed by testing, root-caused while building main.cpp's
 * --maxframes: a TaskParam's SetValue() called from *inside* a plugin's own code, e.g. this file's
 * Process(), does not propagate back to a client's later GetParameter<T>(name).GetValue() call at
 * all - values only flow client -> server for this API, never the reverse, at least not the way
 * this task uses it. This isn't specific to LastEvent's StringTaskParam type either: an
 * Int32TaskParam frame counter added to MotionDetectTask hit the identical symptom. No vendor
 * example demonstrates the reverse direction either. Until a real fix or workaround is found (a
 * DataOutput/DataInput port pair, like ShouldWrite below, is the one already-proven-live channel
 * for server -> client-adjacent data in this codebase - see MotionDetect::c_shouldWriteOutputName),
 * this event relay is effectively non-functional; main.cpp's --maxframes had to be redesigned
 * around an elapsed-time estimate for exactly this reason.
 *
 * Encoder lifecycle: the NVENC session (hw device/frames context, codec context) is opened once in
 * Init() and kept alive for the whole task lifetime, not reopened at each rollover - avcodec_open2
 * (session creation) is genuinely slow (tens-100ms+), and at high frame rates (485fps observed)
 * that's enough time for the upstream queue to back up and drop frames before Process() even
 * finishes handling the first one (measured as a one-time ~100-frame burst before this fix). Only
 * the muxer (AVFormatContext) rotates at each interval: hevc_nvenc declares
 * AV_CODEC_CAP_ENCODER_FLUSH support (verified directly against the installed libavcodec), so a
 * rollover drains the encoder's pending packets into the closing file, calls
 * avcodec_flush_buffers() to reset it for reuse, then opens a fresh muxer for the next file -
 * no new NVENC session, no stall, no dropped frames at rollover boundaries either.
 */
class RecordTask : public eSdkPro::Plugin::TaskWorker
{
public:
    RecordTask();
    virtual ~RecordTask();

    bool Init() override;
    void Deinit() override;
    void Stop() override;

    bool Process() override;

private:
    bool InitEncoder();
    void TeardownEncoder();

    bool RollSegmentIfNeeded(uint64_t timestampUs);
    bool OpenSegment(uint64_t timestampUs);
    void CloseSegment(bool reuseEncoder);
    void CreateSymlink(const std::string& target, const std::string& link);
    bool SaveSnapshot(const eSdkPro::Frame& inputFrame, const std::string& jpgPath);

    bool SendFrameToEncoder(AVFrame* frame);
    bool DrainPackets();

    eSdkPro::Plugin::FrameInput m_input{};
    eSdkPro::Plugin::DataInput m_shouldWriteInput{};

    eSdkPro::Plugin::StringTaskParam m_outputRootParam{};
    eSdkPro::Plugin::StringTaskParam m_nameParam{};
    eSdkPro::Plugin::StringTaskParam m_deviceIdParam{};
    eSdkPro::Plugin::Int32TaskParam m_widthParam{};
    eSdkPro::Plugin::Int32TaskParam m_heightParam{};
    eSdkPro::Plugin::Int32TaskParam m_framerateParam{};
    eSdkPro::Plugin::Int32TaskParam m_bitrateKbpsParam{};
    eSdkPro::Plugin::Int32TaskParam m_newFileIntervalSecParam{};
    eSdkPro::Plugin::StringTaskParam m_presetParam{};
    eSdkPro::Plugin::Int32TaskParam m_minBrightChangeParam{};
    eSdkPro::Plugin::FloatTaskParam m_temperatureParam{};
    eSdkPro::Plugin::StringTaskParam m_ptpStatusParam{};
    eSdkPro::Plugin::StringTaskParam m_cameraConfigNameParam{};
    eSdkPro::Plugin::StringTaskParam m_lastSessionEventParam{};
    eSdkPro::Plugin::StringTaskParam m_lastSegmentClosedParam{};
    eSdkPro::Plugin::StringTaskParam m_lastSegmentStartedParam{};
    eSdkPro::Plugin::StringTaskParam m_lastSnapshotParam{};

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int64_t m_frameIndex = 0; // per-segment pts counter, reset on each OpenSegment

    // Housekeeping/rollover state, matching the old pipeline's exact trigger logic.
    bool m_firstFrame = true;
    uint64_t m_lastRolloverTimestampS = 0;
    bool m_snapshotPending = false; // set by OpenSegment(), consumed by the next Process() call
    uint64_t m_lastWrittenTimestampUs = 0; // for the "# Last capture time" footer on close

    // Persistent (whole task lifetime) encoder state.
    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVBufferRef* m_hwFramesCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;

    // Per-segment muxer/metadata state, rotated at each rollover.
    AVFormatContext* m_formatCtx = nullptr;
    AVStream* m_stream = nullptr;
    std::ofstream m_timestampFile;
    std::string m_stagingMp4Path;
    std::string m_stagingTxtPath;
    std::string m_finalMp4Path;
    std::string m_finalTxtPath;
    std::string m_finalJpgPath;
};
