#pragma once

#include <eSdkPro/plugin/taskworker.h>

#include <stdint.h>
#include <string>

namespace MotionDetect
{
const std::string c_taskName = "MotionDetect";

const std::string c_inputName = "InFrame";
const std::string c_outputName = "OutFrame";
// Companion data port carrying the §3.21 recording decision plus the §3.18 histogram counts
// (see ShouldWritePortPayload below) alongside every frame - see the class doc comment for why
// this exists instead of simply not pushing filtered frames on c_outputName.
const std::string c_shouldWriteOutputName = "ShouldWrite";
// Decimated live-preview port (§3.25): only pushed every LiveRatio-th frame, shown at ~50% of
// the original frame size (see motiondetecttask.cpp's c_previewFinalScale for why the local
// downscale factor isn't simply 0.5x), showing the same fully-composited (bordered/overlaid/
// rotated) content the video encodes - separate from c_outputName so RecordTask's every-frame
// requirement (see class doc comment) is unaffected by preview decimation.
const std::string c_previewOutputName = "PreviewFrame";

// Height, in pixels, of the black region appended above each frame, reserved for the status-bar
// text overlay (site/timestamp/queue depth/etc). Matches the old pipeline's hardcoded
// frameborder=64.
constexpr uint32_t c_borderHeight = 64;

// Task parameter names, configurable per-instance via eCapturePro or Task::GetParameter<T>() -
// equivalent to the old CLI's -s/--site and -n/--name flags.
const std::string c_siteParamName = "Site";
const std::string c_nameParamName = "Name";
// Equivalent to the old CLI's -r/--rotateimage (PROCESSING_SPEC_teeldyne.md §3.19): a fixed
// 90-degree counterclockwise rotation, applied to the content region only (after motion
// detection would consume the unrotated frame, before the border/overlay is composited) - not an
// arbitrary angle, matching the old pipeline exactly.
const std::string c_rotateParamName = "Rotate";
// Equivalent to the old CLI's -b/--minBrightChange (§3.18): must be 20 or 30, selects the
// histogram bin-edge table. Named MinBrightChange (not the old CLI's confusingly-abbreviated
// "-b") to avoid clashing with RecordTask's -b/bitrate in spirit, even though these are separate
// task param namespaces.
const std::string c_minBrightChangeParamName = "MinBrightChange";
// Equivalent to the old CLI's -w/--writeallframes (§3.21): disables recording-decision filtering,
// every frame is pushed downstream regardless of motion.
const std::string c_writeAllFramesParamName = "WriteAllFrames";
// Camera's configured frame rate, needed for the statusFrame heartbeat's "1.5x fps" debounce
// (§3.21) - same value the client also passes to RecordTask's Framerate param.
const std::string c_framerateParamName = "Framerate";
// Equivalent to the old CLI's -l/--liveratio (§3.25): preview a frame every LiveRatio frames.
// Old default 70; division/decimation only, no thread-count divisor since round-robin
// thread-splitting isn't ported (mapping doc Finding B).
const std::string c_liveRatioParamName = "LiveRatio";
// Equivalent to the old CLI's --nopreview (§3.25): skip preview generation entirely.
const std::string c_noPreviewParamName = "NoPreview";
// Number of buffers registered/queued per output port (§4's "no frame-rate throttling" - the new
// pipeline's equivalent of the old bounded frame_queue). With 1 buffer (the vendor examples'
// default and this task's original value), GetOutputFrame()/GetOutputData() has zero slack: a
// momentary downstream slowdown (e.g. NVENC encode-time variance at a higher-quality preset)
// blocks this task's Process() immediately, with nothing absorbing the spike. Raising this gives
// real headroom. m_output and m_shouldWriteOutput are always pushed in lockstep (see class doc),
// so they're always given the *same* count - giving one port more slack than the other would
// just move the blocking bottleneck to whichever port has fewer buffers.
const std::string c_queueDepthParamName = "QueueDepth";
} // namespace MotionDetect

// Wire format for the ShouldWrite companion data port (see MotionDetect::c_shouldWriteOutputName
// above). Kept byte-for-byte identical to Record::ShouldWritePortPayload in recordtask.h -
// duplicated rather than shared via a cross-plugin header, since it's small/stable and this
// project's motion_detect/record plugins otherwise have no shared-header build wiring (see
// build_and_install.sh: each is compiled as an independent .so with its own CMakeLists.txt).
// If you change this, change recordtask.h's copy too.
#pragma pack(push, 1)
struct ShouldWritePortPayload
{
    uint8_t shouldWrite;    // §3.21 recording decision: 0 = skip, 1 = write
    uint32_t queueLength;   // §3.20/§3.24 "Q:"/Queue Length - see QueueDepth comment above
    uint32_t histCounts[7]; // §3.18 step 3 cumulative histogram counts (>= edge[i])
};
#pragma pack(pop)

/**
 * Single consolidated plugin for all per-frame processing between the camera and the encoder,
 * mirroring the old pipeline's storage_worker_cv::run() stage (motion detection, rotation,
 * status-bar overlay, recording decision, metadata) as one task rather than splitting it across
 * several PluginTasks - that per-frame state (histogram bins, moving-pixel flag, etc.) is only
 * ever needed within this one stage, so there's no reason to pay inter-task port overhead moving
 * it between separate plugins.
 *
 * Currently implemented: extends each frame with a border region at the top (black, or gray if
 * nothing moved - §3.20) and draws a single-line status-bar text overlay into it
 * (site | timestamp | name | Q:<queue length> | frame id [| N.R.]), matching the old pipeline's
 * copyMakeBorder + putText stage as far as the data available at this point in the port allows.
 * See `QueueDepth` below for what "Q:" actually measures here. Optional
 * 90-degree counterclockwise rotation (§3.19, the `Rotate` param), applied to the content region
 * before the border/overlay is composited - swaps the output frame's width/height versus the raw
 * camera frame, which the client must account for when sizing RecordTask (see main.cpp).
 *
 * Motion detection (§3.18: absdiff + 7-bin cumulative histogram + per-bin adaptive threshold,
 * see kernel.cu's motionDiffKernel) and recording-decision filtering (§3.21: only frames that are
 * moving, the first frame, or a periodic ~10s heartbeat should be written) are both implemented.
 * The write/skip decision is communicated to RecordTask via a companion `ShouldWrite` DataOutput
 * port, one byte per frame, rather than by simply not pushing filtered frames on the frame output
 * port - eSDK Pro's FrameOutput requires PushFrame() every Process() call for a registered buffer
 * to be recycled (GetOutputFrame() blocks forever otherwise, confirmed by testing: skipping
 * PushFrame() on filtered frames starved the single registered output buffer within a few frames
 * and stalled the whole camera pipeline, visible as a runaway "Missed Save" counter). So this
 * task pushes every frame *and* a same-cadence ShouldWrite byte unconditionally; RecordTask reads
 * both per Process() call and decides there whether to actually encode/write. A side benefit:
 * RecordTask now sees every frame's timestamp for its own rollover-boundary check regardless of
 * the write decision, so (unlike an earlier design of this feature) a segment's first frame being
 * filtered out no longer delays that segment's rollover trigger.
 *
 * `QueueDepth` (default 8): how many buffers m_output/m_shouldWriteOutput each pre-register -
 * this is the new pipeline's equivalent of the old bounded frame_queue between capture and
 * storage threads, giving Process() slack to keep running even if RecordTask's encode step is
 * momentarily slow, instead of blocking immediately on a single buffer. Unlike the old
 * frame_queue, there's no exposed depth *counter* to log per frame (no eSDK Pro API for that,
 * confirmed by checking) - just this fixed pre-allocated pool size. Tested at 485fps with
 * QueueDepth 1 vs. 4 at both p4 (clean either way on this hardware) and p7 (overloads at the same
 * rate either way, "Missed Save" growing linearly for the whole run): raising this does NOT
 * rescue a preset whose NVENC encode time is *sustainedly* too slow for the frame rate - it only
 * absorbs bursty/transient slowdowns. Kept at 8 (not 1) anyway as a safety margin for exactly
 * that transient case, which this session's tests weren't set up to isolate from the sustained
 * case.
 *
 * Live preview (§3.25): every LiveRatio-th frame (default 70, matching the old CLI's -l/
 * --liveratio; 0/--nopreview disables entirely) is downscaled on GPU (kernel.cu's
 * downscaleKernel, nearest-neighbor) to end up shown at ~50% of the original frame size (see
 * motiondetecttask.cpp's c_previewFinalScale) and pushed on a dedicated `PreviewFrame` port to an
 * eSDK Pro `ImageDisplayTask`, which relays it to a client-supplied callback running in the
 * *client* process (main.cpp) - unlike everything else in this class, that callback isn't
 * constrained by this being a server-side plugin, so the client shows it in a real cv::imshow
 * window, same as the old pipeline. Decimation happens by simply not calling
 * GetOutputFrame()/PushFrame() on skipped frames - safe, unlike skipping PushFrame() on an
 * already-fetched buffer (see the ShouldWrite paragraph above for why that specific pattern
 * deadlocks); PreviewFrame's own registered buffer is only ever touched on frames that are
 * actually going to be pushed.
 *
 * Deferred (not implemented yet, need a later stage first):
 *  - Queue depth: no eSDK Pro port-queue-depth query found (mapping doc gap list #3).
 *  - Histogram bin / move% overlay fields: motion detection now exists, but these aren't
 *    surfaced into the status-bar text yet (move% is a per-output-file cumulative stat that would
 *    need file-rollover awareness this task doesn't have).
 *  - Thread id: dropped - round-robin thread-splitting isn't being ported (mapping doc Finding B).
 *  - Exposure/gain: needs camera parameter access, which this frame-only plugin doesn't have yet.
 */
class MotionDetectTask : public eSdkPro::Plugin::TaskWorker
{
public:
    MotionDetectTask();
    virtual ~MotionDetectTask();

    bool Init() override;
    void Deinit() override;

    bool Process() override;

private:
    // I/O ports
    eSdkPro::Plugin::FrameInput m_input{};
    eSdkPro::Plugin::FrameOutput m_output{};
    eSdkPro::Plugin::DataOutput m_shouldWriteOutput{};
    eSdkPro::Plugin::FrameOutput m_previewOutput{};

    // Parameters
    eSdkPro::Plugin::StringTaskParam m_siteParam{};
    eSdkPro::Plugin::StringTaskParam m_nameParam{};
    eSdkPro::Plugin::BoolTaskParam m_rotateParam{};
    eSdkPro::Plugin::Int32TaskParam m_minBrightChangeParam{};
    eSdkPro::Plugin::BoolTaskParam m_writeAllFramesParam{};
    eSdkPro::Plugin::Int32TaskParam m_framerateParam{};
    eSdkPro::Plugin::Int32TaskParam m_liveRatioParam{};
    eSdkPro::Plugin::BoolTaskParam m_noPreviewParam{};
    eSdkPro::Plugin::Int32TaskParam m_queueDepthParam{};

    // Reusable device buffer for the per-frame status text, uploaded once per Process() call and
    // read by the text-drawing kernel. Allocated once in Init(), not per frame.
    char* m_deviceTextBuffer = nullptr;
    static constexpr uint32_t c_maxTextLen = 200;

    // Motion-detection state (§3.18), persisted across Process() calls for this camera. Allocated
    // lazily on the first frame, once the raw camera frame dimensions are known.
    uint8_t* m_prevFrameBuf = nullptr;
    size_t m_prevFramePitch = 0;
    uint32_t* m_deviceHistCounts = nullptr; // 7 uint32_t bin counts, see kernel.cuh

    // Recording-decision state (§3.21).
    bool m_firstFrame = true;
    uint32_t m_framesSinceLastStatusFrame = 0;

    // Live-preview decimation state (§3.25).
    uint32_t m_frameCounter = 0;
};
