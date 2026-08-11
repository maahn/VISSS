#include "motiondetecttask.h"

#include "kernel.cuh"

#include <eSdkPro/errors.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// This plugin is loaded by the eCaptureProServer daemon as a shared library (dropped into
// eSdkPro/plugins/), not compiled into the client app - ConnectServer(ip) always talks to that
// separate process, even for 127.0.0.1, so registration must happen when the .so is loaded, same
// pattern as the cuda_brightness example.
class RegisterMotionDetectTask
{
public:
    RegisterMotionDetectTask()
    {
        try
        {
            eSdkPro::Plugin::RegisterTaskPlugin<MotionDetectTask>();
        }
        catch (const std::exception&)
        {}
    }
};
static RegisterMotionDetectTask g_registerMotionDetectTask{};

namespace
{
// Desired FINAL preview size, relative to the original (pre-downscale) composited frame -
// matches the old pipeline's §3.25 semantic ("downscaled 0.4x", meaning 0.4x of the real frame).
// eSDK Pro's ImageDisplayTask silently applies its own additional ~0.5x reduction on top of
// whatever's pushed to it, confirmed by testing at two different requested sizes (0.4x and 0.5x
// locally both arrived at the client scaled down by another consistent, exact 0.5x - e.g.
// requesting 812px wide delivered 406px). So what we request locally must be
// c_previewFinalScale / c_previewSdkReductionFactor to make the size actually shown match this
// constant, not naively c_previewFinalScale itself.
constexpr double c_previewFinalScale = 0.5;
constexpr double c_previewSdkReductionFactor = 0.5;

// Must match recordtask.cpp's identical c_minSecondsBeforeRollover exactly - see this task's
// doRollover comment below for why. Duplicated rather than shared via a header since each plugin
// is an independent CMake project/.so (same reasoning as ShouldWritePortPayload) - if you change
// one, change both.
constexpr uint64_t c_minSecondsBeforeRollover = 10;

// YYYY/MM/DD HH:MM:SS.mmm, local time zone - matches the old pipeline's overlay format exactly
// (PROCESSING_SPEC_teeldyne.md §3.20's formatUnixTimeMicros; local time there is deliberate,
// distinct from file naming which uses UTC per §3.22).
std::string formatTimestamp(uint64_t timestampNs)
{
    const time_t seconds = static_cast<time_t>(timestampNs / 1000000000ULL);
    const uint32_t millis = static_cast<uint32_t>((timestampNs / 1000000ULL) % 1000ULL);

    std::tm tmLocal{};
    localtime_r(&seconds, &tmLocal);

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d.%03u", tmLocal.tm_year + 1900, tmLocal.tm_mon + 1,
                  tmLocal.tm_mday, tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec, millis);
    return std::string(buf);
}
} // namespace

MotionDetectTask::MotionDetectTask()
{
    SetName(MotionDetect::c_taskName);

    m_input = CreateFrameInput(MotionDetect::c_inputName, eSdkPro::HWPlatform::Cuda);
    m_output = CreateFrameOutput(MotionDetect::c_outputName, eSdkPro::HWPlatform::Cuda);
    m_shouldWriteOutput = CreateDataOutput(MotionDetect::c_shouldWriteOutputName, eSdkPro::HWPlatform::Host);
    // Host, not Cuda: the port's platform is a static contract set here, separate from whatever
    // individual Frame objects get pushed through it later - Process() downloads the downscaled
    // preview to host before pushing (see its comment for why).
    m_previewOutput = CreateFrameOutput(MotionDetect::c_previewOutputName, eSdkPro::HWPlatform::Host);

    // Matches the old CLI's -s/--site (default "none") and -n/--name (default "VISSS").
    m_siteParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(MotionDetect::c_siteParamName);
    m_siteParam.SetValue("none");
    m_siteParam.SetToolTip("Site name shown in the status-bar overlay text.");

    m_nameParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(MotionDetect::c_nameParamName);
    m_nameParam.SetValue("VISSS");
    m_nameParam.SetToolTip("Camera/instrument name shown in the status-bar overlay text.");

    m_rotateParam = CreateParameter<eSdkPro::Plugin::BoolTaskParam>(MotionDetect::c_rotateParamName);
    m_rotateParam.SetValue(false);
    m_rotateParam.SetToolTip("Rotate the content region 90 degrees counterclockwise before the border/overlay is "
                             "composited (matches the old CLI's -r/--rotateimage).");

    // Matches the old CLI's -b/--minBrightChange; only 20 or 30 are valid (checked in Init()).
    m_minBrightChangeParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(MotionDetect::c_minBrightChangeParamName);
    m_minBrightChangeParam.SetValue(20);
    m_minBrightChangeParam.SetToolTip("Motion-detection histogram bin-edge table selector; must be 20 or 30.");

    // Matches the old CLI's -w/--writeallframes.
    m_writeAllFramesParam = CreateParameter<eSdkPro::Plugin::BoolTaskParam>(MotionDetect::c_writeAllFramesParamName);
    m_writeAllFramesParam.SetValue(false);
    m_writeAllFramesParam.SetToolTip(
        "Disable recording-decision filtering: push every frame downstream regardless of motion.");

    m_framerateParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(MotionDetect::c_framerateParamName);
    m_framerateParam.SetValue(0);
    m_framerateParam.SetToolTip("Camera's configured frame rate, used for the statusFrame heartbeat's debounce.");

    // Matches the old CLI's -l/--liveratio (default 70).
    m_liveRatioParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(MotionDetect::c_liveRatioParamName);
    m_liveRatioParam.SetValue(70);
    m_liveRatioParam.SetToolTip("Preview a frame every this-many frames.");

    // Matches the old CLI's --nopreview.
    m_noPreviewParam = CreateParameter<eSdkPro::Plugin::BoolTaskParam>(MotionDetect::c_noPreviewParamName);
    m_noPreviewParam.SetValue(false);
    m_noPreviewParam.SetToolTip("Disable live preview generation entirely.");

    m_queueDepthParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(MotionDetect::c_queueDepthParamName);
    m_queueDepthParam.SetValue(8);
    m_queueDepthParam.SetToolTip(
        "Buffers pre-registered per output port - slack to absorb momentary downstream slowdowns.");

    // Matches RecordTask's own NewFileIntervalSec - see MotionDetect::c_newFileIntervalSecParamName.
    m_newFileIntervalSecParam =
        CreateParameter<eSdkPro::Plugin::Int32TaskParam>(MotionDetect::c_newFileIntervalSecParamName);
    m_newFileIntervalSecParam.SetValue(600);
    m_newFileIntervalSecParam.SetToolTip(
        "Same value as RecordTask's NewFileIntervalSec - resets the \"M:\" move-percent overlay "
        "stat at the same boundaries RecordTask rolls over a file, without any cross-task "
        "signaling (both derive the same boundary from the same frame timestamps).");
}

MotionDetectTask::~MotionDetectTask()
{}

bool MotionDetectTask::Init()
{
    // Matches the old CLI's fatal validation of --minBrightChange (visss-data-acquisition.cpp):
    // only 20 or 30 select a defined bin-edge table.
    const int32_t minBrightChange = m_minBrightChangeParam.GetValue();
    if (minBrightChange != 20 && minBrightChange != 30)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "MinBrightChange must be 20 or 30, got " + std::to_string(minBrightChange));
        return false;
    }

    int32_t queueDepth = m_queueDepthParam.GetValue();
    if (queueDepth < 1)
    {
        LogMessage(eSdkPro::LogLevel::Warning, "QueueDepth must be >= 1, got " + std::to_string(queueDepth) +
                                                    " - clamping to 1.");
        queueDepth = 1;
    }

    // Register QueueDepth placeholder empty output frames; real dimensions are set lazily once
    // the first input frame arrives (same buffer-reuse pattern as the cuda_brightness example,
    // just with QueueDepth buffers instead of that example's 1 - see class doc comment on
    // `QueueDepth`). m_output and m_shouldWriteOutput must register the *same* count - they're
    // always pushed in lockstep, so mismatched depths would just move the blocking bottleneck to
    // whichever port has fewer buffers.
    for (int32_t i = 0; i < queueDepth; i++)
    {
        auto frame = eSdkPro::Frame();
        m_output.RegisterOutputFrame(frame);
        m_output.QueueOutputFrame(frame);
    }

    for (int32_t i = 0; i < queueDepth; i++)
    {
        auto shouldWriteData = eSdkPro::Data(sizeof(ShouldWritePortPayload), eSdkPro::HWPlatform::Host);
        m_shouldWriteOutput.RegisterOutputData(shouldWriteData);
        m_shouldWriteOutput.QueueOutputData(shouldWriteData);
    }

    // Placeholder preview frame, same lazy-resize pattern as m_output (§3.25).
    auto previewFrame = eSdkPro::Frame();
    m_previewOutput.RegisterOutputFrame(previewFrame);
    m_previewOutput.QueueOutputFrame(previewFrame);

    cudaError_t err = cudaMalloc(&m_deviceTextBuffer, c_maxTextLen);
    if (err != cudaSuccess)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "Failed to allocate device text buffer: " + std::string(cudaGetErrorString(err)));
        return false;
    }

    err = cudaMalloc(&m_deviceHistCounts, 7 * sizeof(uint32_t));
    if (err != cudaSuccess)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "Failed to allocate device histogram buffer: " + std::string(cudaGetErrorString(err)));
        return false;
    }

    m_firstFrame = true;
    m_framesSinceLastStatusFrame = 0;
    m_frameCounter = 0;
    m_frameCountInFile = 0;
    m_frameCountMoving = 0;
    m_lastRolloverTimestampS = 0;
    m_startTimestampS = 0;

    return true;
}

void MotionDetectTask::Deinit()
{
    if (m_deviceTextBuffer != nullptr)
    {
        cudaFree(m_deviceTextBuffer);
        m_deviceTextBuffer = nullptr;
    }
    if (m_deviceHistCounts != nullptr)
    {
        cudaFree(m_deviceHistCounts);
        m_deviceHistCounts = nullptr;
    }
    if (m_prevFrameBuf != nullptr)
    {
        cudaFree(m_prevFrameBuf);
        m_prevFrameBuf = nullptr;
    }
}

bool MotionDetectTask::Process()
{
    try
    {
        eSdkPro::Frame inputFrame = m_input.GetFrame();

        if (inputFrame.GetPixelFormat() != GVSP_PIX_MONO8)
        {
            LogMessage(eSdkPro::LogLevel::Error, "MotionDetectTask only supports GVSP_PIX_MONO8 frames currently.");
            return false;
        }

        const uint32_t width = inputFrame.GetWidth();
        const uint32_t height = inputFrame.GetHeight();

        // Lazily allocate the previous-frame buffer once dimensions are known, zero-initialized
        // to match the old pipeline's zero-initialized imgOld on a worker's very first frame.
        if (m_prevFrameBuf == nullptr)
        {
            cudaError_t allocErr = cudaMallocPitch(&m_prevFrameBuf, &m_prevFramePitch, width, height);
            if (allocErr != cudaSuccess)
            {
                throw eSdkPro::ESdkProException(
                    eSdkPro::ErrorCode::General,
                    "cudaMallocPitch failed for previous-frame buffer: " + std::string(cudaGetErrorString(allocErr)));
            }
            allocErr = cudaMemset2D(m_prevFrameBuf, m_prevFramePitch, 0, width, height);
            if (allocErr != cudaSuccess)
            {
                throw eSdkPro::ESdkProException(eSdkPro::ErrorCode::General,
                                                "cudaMemset2D failed to zero-init previous-frame buffer: " +
                                                    std::string(cudaGetErrorString(allocErr)));
            }
        }

        // Motion detection (§3.18) - must run on the raw, pre-rotation frame; updates
        // m_prevFrameBuf ("imgOld") every frame regardless of the diff outcome.
        launchMotionDiffKernel(inputFrame.GetDataPtr(), inputFrame.GetStride(), m_prevFrameBuf,
                               static_cast<uint32_t>(m_prevFramePitch), width, height,
                               m_minBrightChangeParam.GetValue() == 30, m_deviceHistCounts);

        uint32_t histCounts[7];
        cudaError_t err = cudaMemcpy(histCounts, m_deviceHistCounts, sizeof(histCounts), cudaMemcpyDeviceToHost);
        if (err != cudaSuccess)
        {
            throw eSdkPro::ESdkProException(
                eSdkPro::ErrorCode::General,
                "cudaMemcpy failed reading back histogram counts: " + std::string(cudaGetErrorString(err)));
        }

        // Cumulative-from-the-top histogram + per-bin adaptive threshold (§3.18 steps 3-4) - an
        // exact port, deliberately including the integer-division-then-floor-of-2 quirk that
        // makes bins 3-6 all share the same threshold (2).
        for (int i = 5; i >= 0; i--)
        {
            histCounts[i] += histCounts[i + 1];
        }
        bool movingPixel = false;
        bool movingPixels[7] = {false, false, false, false, false, false, false};
        int minMovingPixel = 20;
        int tt = 1;
        for (int bin = 0; bin < 7; bin++)
        {
            int movingPixelThreshold = minMovingPixel / tt;
            if (movingPixelThreshold < 2)
            {
                movingPixelThreshold = 2;
            }
            if (histCounts[bin] >= static_cast<uint32_t>(movingPixelThreshold))
            {
                movingPixel = true;
                movingPixels[bin] = true;
            }
            tt *= 2;
        }

        const bool isFirstFrame = m_firstFrame;
        m_firstFrame = false;

        // Recording-decision heartbeat (§3.21): fires roughly once per 10 wall-clock seconds,
        // frame-count debounced (not wall-clock-time debounced) exactly like the old pipeline -
        // "thread_id==0" is trivially always true here since round-robin thread-splitting isn't
        // ported (mapping doc Finding B).
        const uint64_t timestampS = inputFrame.GetTimestampNs() / 1000000000ULL;
        const int32_t framerate = m_framerateParam.GetValue();
        bool statusFrame = false;
        if ((timestampS % 10 == 0) && (framerate > 0) &&
            (static_cast<double>(m_framesSinceLastStatusFrame) > 1.5 * framerate))
        {
            statusFrame = true;
            m_framesSinceLastStatusFrame = 0;
        }
        else
        {
            m_framesSinceLastStatusFrame++;
        }

        const bool shouldWrite = m_writeAllFramesParam.GetValue() || movingPixel || isFirstFrame || statusFrame;

        // "M:" move-percent overlay stat (§3.20/§3.13): independently derives the same
        // file-rollover boundary RecordTask uses (RollSegmentIfNeeded in recordtask.cpp) from the
        // same frame timestamps, so the two tasks' resets stay in sync without any cross-task
        // signaling - there is no port carrying data from RecordTask back to this task, so this
        // is the only way to get per-file semantics here. Simplification vs. the old pipeline:
        // the reset happens *before* this frame's text is built (old pipeline's reset happened
        // after, so a new file's first frame showed the outgoing file's trailing M% for one
        // frame) - the new file's first frame here shows a freshly-reset 0.0% instead, which is
        // simpler and avoids that one-frame artifact.
        if (isFirstFrame)
        {
            m_startTimestampS = timestampS;
        }
        const int32_t newFileIntervalSec = m_newFileIntervalSecParam.GetValue();
        // Suppressed within c_minSecondsBeforeRollover seconds of this task's first frame - kept
        // in sync with RecordTask's identical suppression (recordtask.cpp's
        // RollSegmentIfNeeded/c_minSecondsBeforeRollover), so this task's own M:/H: reset lands on
        // the same frame RecordTask actually opens a new segment on, not a boundary RecordTask
        // itself skipped as a spurious near-immediate second rollover.
        const bool doRollover = (newFileIntervalSec > 0) &&
                                 (timestampS % static_cast<uint64_t>(newFileIntervalSec) == 0) &&
                                 (timestampS != m_lastRolloverTimestampS) &&
                                 (timestampS >= m_startTimestampS + c_minSecondsBeforeRollover);
        if (doRollover || isFirstFrame)
        {
            m_frameCountInFile = 0;
            m_frameCountMoving = 0;
            m_lastRolloverTimestampS = timestampS;
        }
        const double movePercent =
            static_cast<double>(m_frameCountMoving) * 100.0 / static_cast<double>(m_frameCountInFile + 1);
        char movePercentBuf[16];
        std::snprintf(movePercentBuf, sizeof(movePercentBuf), "%.1f", movePercent);

        m_frameCountInFile++;
        // Tracks actual detected motion (movingPixel), not shouldWrite: with WriteAllFrames on,
        // shouldWrite is unconditionally true for every frame (see above), which would make M:
        // a meaningless, permanently-100% stat instead of telling the operator anything about how
        // much of the scene is actually moving - the old pipeline had this exact same quirk
        // (storage_worker_cv.h's frame_count_moving also counted on its own writeallframes-ORed
        // write decision, not on movingPixel alone), but the project owner flagged it as wrong
        // rather than something to faithfully reproduce (2026-08-11) - M: is supposed to answer
        // "how much motion is happening," which shouldWrite doesn't when write-filtering is off.
        if (movingPixel)
        {
            m_frameCountMoving++;
        }

        // A 90-degree rotation swaps width/height for the content region - the border still sits
        // on top of the (now rotated) content, matching the old pipeline's order: rotate (§3.19)
        // happens after motion detection, before border/overlay compositing (§3.20).
        const bool rotate = m_rotateParam.GetValue();
        const uint32_t contentWidth = rotate ? height : width;
        const uint32_t contentHeight = rotate ? width : height;
        const uint32_t outHeight = contentHeight + MotionDetect::c_borderHeight;

        // Get the already allocated output frame, reallocating if its dimensions/format are stale
        // (same buffer-reuse pattern as cuda_brightness). Timed: how long this blocks is this
        // pipeline's queue-length signal (§3.20/§3.24's "Q:"/"Queue Length" - see class doc
        // comment on QueueDepth for why this, not a literal buffer-occupancy count, is what's
        // available here). GetOutputFrame() only blocks once all QueueDepth registered buffers
        // are checked out downstream, i.e. once RecordTask has fallen behind by that many frames -
        // converting the blocked duration into frame-equivalent units gives an honest (if
        // coarse-grained below QueueDepth) estimate of how far behind RecordTask currently is.
        const auto getFrameStart = std::chrono::steady_clock::now();
        auto outputFrame = m_output.GetOutputFrame();
        const auto getFrameBlockedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - getFrameStart)
                .count();
        uint32_t queueLength = 0;
        if (framerate > 0)
        {
            const double frameIntervalUs = 1000000.0 / static_cast<double>(framerate);
            queueLength = static_cast<uint32_t>(static_cast<double>(getFrameBlockedUs) / frameIntervalUs);
        }

        if (outputFrame.GetWidth() != contentWidth || outputFrame.GetHeight() != outHeight ||
            outputFrame.GetPixelFormat() != inputFrame.GetPixelFormat())
        {
            m_output.DeregisterOutputFrame(outputFrame);
            outputFrame =
                eSdkPro::Frame(contentWidth, outHeight, inputFrame.GetPixelFormat(), eSdkPro::HWPlatform::Cuda);
            m_output.RegisterOutputFrame(outputFrame);
        }

        // Mono8 is 1 byte/pixel, so row width in bytes == width in pixels for both cudaMemset2D and
        // cudaMemcpy2D below. Frames may be allocated with device-specific row padding, hence using
        // GetStride() rather than assuming stride == width.
        uint8_t* dstBase = outputFrame.GetDataPtr();
        const size_t dstPitch = outputFrame.GetStride();

        // Border color (§3.20): black (0) if something moved or this is the very first frame,
        // gray (100) otherwise - a purely visual "nothing happened in this frame" flag.
        const uint8_t borderColor = (movingPixel || isFirstFrame) ? 0 : 100;
        err = cudaMemset2D(dstBase, dstPitch, borderColor, contentWidth, MotionDetect::c_borderHeight);
        if (err != cudaSuccess)
        {
            throw eSdkPro::ESdkProException(
                eSdkPro::ErrorCode::General,
                "cudaMemset2D failed for status bar border: " + std::string(cudaGetErrorString(err)));
        }

        // Input frame (rotated if requested) copied below the border.
        uint8_t* dstImage = dstBase + (static_cast<size_t>(MotionDetect::c_borderHeight) * dstPitch);
        if (rotate)
        {
            launchRotate90CcwKernel(inputFrame.GetDataPtr(), inputFrame.GetStride(), width, height, dstImage,
                                    dstPitch);
        }
        else
        {
            err = cudaMemcpy2D(dstImage, dstPitch, inputFrame.GetDataPtr(), inputFrame.GetStride(), width, height,
                                cudaMemcpyDeviceToDevice);
            if (err != cudaSuccess)
            {
                throw eSdkPro::ESdkProException(
                    eSdkPro::ErrorCode::General,
                    "cudaMemcpy2D failed for status bar frame copy: " + std::string(cudaGetErrorString(err)));
            }
        }

        // Status-bar text (site | timestamp | name | Q:<queue> | H:<edge>[N.R.] | M:<move%>),
        // matching the old overlay's field set/order (storage_worker_cv.h) except the trailing
        // thread-id field, dropped since round-robin thread-splitting isn't ported (Finding B).
        // "H:" is the highest-triggered histogram bin's edge value (3-digit zero-padded, empty if
        // nothing moved this frame) - old pipeline scans bins from highest to lowest and takes the
        // first (i.e. highest) one whose adaptive threshold was exceeded.
        std::string edgeStr;
        {
            const bool useThirty = m_minBrightChangeParam.GetValue() == 30;
            const int* edges = useThirty ? c_binEdges30 : c_binEdges20;
            for (int bin = 6; bin >= 0; bin--)
            {
                if (movingPixels[bin])
                {
                    std::string edgeDigits = std::to_string(edges[bin]);
                    edgeStr = std::string(3 - std::min<size_t>(3, edgeDigits.size()), '0') + edgeDigits;
                    break;
                }
            }
        }

        std::string text = m_siteParam.GetValue() + " | " + formatTimestamp(inputFrame.GetTimestampNs()) + " | " +
                            m_nameParam.GetValue() + " | Q:" + std::to_string(queueLength) + " | H:" + edgeStr;
        if (!movingPixel)
        {
            text += " N.R.";
        }
        text += " | M:" + std::string(movePercentBuf) + "%";
        const uint32_t textLen = std::min<uint32_t>(static_cast<uint32_t>(text.size()), c_maxTextLen);

        err = cudaMemcpy(m_deviceTextBuffer, text.data(), textLen, cudaMemcpyHostToDevice);
        if (err != cudaSuccess)
        {
            throw eSdkPro::ESdkProException(
                eSdkPro::ErrorCode::General,
                "cudaMemcpy failed for status bar text upload: " + std::string(cudaGetErrorString(err)));
        }

        // Roughly vertically centered in the border, small left margin. Uses the *rendered*
        // (post-c_fontScale) cell height, not the raw 16x28 bitmap size, to center correctly.
        const uint32_t originX = 10;
        const uint32_t originY = (MotionDetect::c_borderHeight > c_fontRenderedCellHeight)
                                      ? (MotionDetect::c_borderHeight - c_fontRenderedCellHeight) / 2
                                      : 0;
        launchDrawTextKernel(outputFrame, m_deviceTextBuffer, textLen, originX, originY);

        outputFrame.SetFrameId(inputFrame.GetFrameId());
        outputFrame.SetTimestampNs(inputFrame.GetTimestampNs());

        // Live preview (§3.25): decimated, so only fetch/push the PreviewFrame port's buffer on
        // frames actually being previewed - never fetching it on skipped frames is what keeps
        // this safe (see class doc comment's contrast with the ShouldWrite deadlock).
        m_frameCounter++;
        const int32_t liveRatio = m_liveRatioParam.GetValue();
        if (!m_noPreviewParam.GetValue() && liveRatio > 0 && (m_frameCounter % static_cast<uint32_t>(liveRatio) == 0))
        {
            const double previewRequestScale = c_previewFinalScale / c_previewSdkReductionFactor;
            const uint32_t previewWidth = std::max<uint32_t>(1, static_cast<uint32_t>(contentWidth * previewRequestScale));
            const uint32_t previewHeight = std::max<uint32_t>(1, static_cast<uint32_t>(outHeight * previewRequestScale));

            // Downscale into a throwaway device buffer, then download to host before handing off
            // to ImageDisplayTask - it defaults to expecting Host-platform frames (confirmed by
            // testing: connecting a Cuda PreviewFrame port directly failed pipeline startup with
            // "Platform mismatch" until SetGpuDeviceId() was added to the display task, and even
            // then the delivered preview was visibly corrupted/striped - a device-side pitch
            // handling issue in that path this task doesn't control). Only fires once every
            // LiveRatio frames, so the extra allocation here is negligible.
            uint8_t* deviceScratch = nullptr;
            size_t deviceScratchPitch = 0;
            cudaError_t scratchErr = cudaMallocPitch(&deviceScratch, &deviceScratchPitch, previewWidth, previewHeight);
            if (scratchErr != cudaSuccess)
            {
                throw eSdkPro::ESdkProException(
                    eSdkPro::ErrorCode::General,
                    "cudaMallocPitch failed for preview scratch buffer: " + std::string(cudaGetErrorString(scratchErr)));
            }

            launchDownscaleKernel(outputFrame.GetDataPtr(), outputFrame.GetStride(), contentWidth, outHeight,
                                  deviceScratch, static_cast<uint32_t>(deviceScratchPitch), previewWidth,
                                  previewHeight);

            auto previewFrame = m_previewOutput.GetOutputFrame();
            if (previewFrame.GetWidth() != previewWidth || previewFrame.GetHeight() != previewHeight ||
                previewFrame.GetPixelFormat() != outputFrame.GetPixelFormat())
            {
                m_previewOutput.DeregisterOutputFrame(previewFrame);
                previewFrame = eSdkPro::Frame(previewWidth, previewHeight, outputFrame.GetPixelFormat(),
                                              eSdkPro::HWPlatform::Host);
                m_previewOutput.RegisterOutputFrame(previewFrame);
            }

            cudaError_t copyErr =
                cudaMemcpy2D(previewFrame.GetDataPtr(), previewFrame.GetStride(), deviceScratch, deviceScratchPitch,
                            previewWidth, previewHeight, cudaMemcpyDeviceToHost);
            cudaFree(deviceScratch);
            if (copyErr != cudaSuccess)
            {
                throw eSdkPro::ESdkProException(eSdkPro::ErrorCode::General,
                                                "cudaMemcpy2D (device to host) failed for preview frame: " +
                                                    std::string(cudaGetErrorString(copyErr)));
            }

            previewFrame.SetFrameId(inputFrame.GetFrameId());
            previewFrame.SetTimestampNs(inputFrame.GetTimestampNs());
            m_previewOutput.PushFrame(previewFrame);
        }

        // Frame and ShouldWrite decision are both pushed every cycle, unconditionally - required
        // so the registered output buffers keep cycling back through Get.../Push... (see class
        // doc comment). RecordTask reads ShouldWrite and decides there whether to actually
        // encode/write this frame, and reuses histCounts directly for the §3.24 metadata columns.
        auto shouldWriteData = m_shouldWriteOutput.GetOutputData();
        auto* payload = reinterpret_cast<ShouldWritePortPayload*>(shouldWriteData.GetDataPtr());
        payload->shouldWrite = shouldWrite ? 1 : 0;
        payload->queueLength = queueLength;
        std::memcpy(payload->histCounts, histCounts, sizeof(histCounts));
        m_shouldWriteOutput.PushData(shouldWriteData);

        m_output.PushFrame(outputFrame);
    }
    catch (const eSdkPro::ESdkProException& e)
    {
        Abort(e.what());
        return false;
    }
    catch (const std::exception& e)
    {
        Abort(e.what());
        return false;
    }

    return true;
}
