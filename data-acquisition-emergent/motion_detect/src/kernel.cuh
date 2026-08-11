#pragma once

#include <eSdkPro/frame.h>

#include <stdint.h>

// Font geometry, shared by kernel.cu (which also includes font16x28.h for the actual glyph data)
// and callers that need to compute layout (e.g. centering text within the status-bar border).
// Plain constexpr, safe to include from non-CUDA .cpp files - unlike font16x28.h, which uses the
// CUDA __constant__ attribute and must only be included from a .cu file.
//
// 16x28, not the original 8x14: the original font was a tiny 8x14 bitmap nearest-neighbor
// upscaled 2x at draw time (c_fontScale below), which looked visibly blocky/low-detail on real
// footage - project owner flagged it as "still from the old resolution" (2026-08-11). Regenerated
// (gen_font16x28.py, DejaVuSansMono-Bold.ttf, same source font as before) natively at 16x28
// instead - same on-screen size as before (8x14 * scale 2 == 16x28 * scale 1), genuinely more
// glyph detail since every pixel is a real render at that resolution, not a doubled-up blocky one.
constexpr uint32_t c_fontCellWidth = 16;
constexpr uint32_t c_fontCellHeight = 28;
constexpr int c_fontFirstChar = 0x20;
constexpr int c_fontLastChar = 0x7E;

// Host-visible copies of kernel.cu's g_binEdges20/g_binEdges30 __constant__ arrays (the __device__
// qualifier means those aren't visible outside CUDA-compiled code) - motiondetecttask.cpp needs
// the same edge values to format the status-bar "H:" field (PROCESSING_SPEC_teeldyne.md §3.18's
// "the exact, hardcoded values to preserve"). Keep these byte-for-byte identical to kernel.cu's
// copies - if you change one, change both.
constexpr int c_binEdges20[8] = {20, 30, 40, 60, 80, 100, 120, 256};
constexpr int c_binEdges30[8] = {30, 40, 60, 80, 100, 120, 140, 256};

// Nearest-neighbor upscale factor applied when drawing (each glyph bitmap bit becomes an NxN
// block of output pixels) - the font data itself (font16x28.h) stays a fixed 16x28 bitmap;
// c_fontScale is the only thing to change to make the status-bar text bigger/smaller on screen.
// Default 1: the font is already natively rendered at the on-screen size the old 8x14 font needed
// scale=2 to reach, so no further (blocky) magnification is needed by default - raise this only
// if even-bigger text is wanted, at the cost of the same blockiness this change was meant to fix.
constexpr uint32_t c_fontScale = 1;
// Rendered (on-screen) glyph cell size - what callers doing layout (e.g. centering text within
// the status-bar border) should use, not c_fontCellWidth/Height, which is the raw bitmap size.
constexpr uint32_t c_fontRenderedCellWidth = c_fontCellWidth * c_fontScale;
constexpr uint32_t c_fontRenderedCellHeight = c_fontCellHeight * c_fontScale;

/**
 * Draws ASCII text into an output frame using a fixed 8x14 monospace bitmap font, writing white
 * (255) pixels for "on" glyph pixels and leaving everything else untouched (so callers should
 * fill the background/border color first).
 * @param outputFrame The frame to draw into (GVSP_PIX_MONO8, HWPlatform::Cuda).
 * @param deviceText Pointer to device memory holding at least textLen bytes of ASCII text.
 * @param textLen Number of characters to draw.
 * @param originX Left edge, in pixels, of the first character.
 * @param originY Top edge, in pixels, of the text row.
 */
void launchDrawTextKernel(eSdkPro::Frame& outputFrame, const char* deviceText, uint32_t textLen, uint32_t originX,
                          uint32_t originY);

/**
 * Rotates a MONO8 device image 90 degrees counterclockwise (matching the old pipeline's
 * cv::rotate(..., ROTATE_90_COUNTERCLOCKWISE), PROCESSING_SPEC_teeldyne.md §3.19), writing
 * directly into a sub-region of an already-allocated destination buffer (e.g. the content area
 * below the status-bar border) rather than a standalone frame, so no extra allocation/copy is
 * needed to compose it with the border.
 * @param srcBuf Device pointer to the source image (width=srcWidth, height=srcHeight).
 * @param srcPitch Row stride, in bytes, of the source buffer.
 * @param srcWidth Width, in pixels, of the source image.
 * @param srcHeight Height, in pixels, of the source image.
 * @param dstBuf Device pointer to the destination's top-left pixel. The rotated image is
 * srcHeight wide and srcWidth tall.
 * @param dstPitch Row stride, in bytes, of the destination buffer.
 */
void launchRotate90CcwKernel(const uint8_t* srcBuf, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight,
                             uint8_t* dstBuf, uint32_t dstPitch);

/**
 * Motion detection core (PROCESSING_SPEC_teeldyne.md §3.18), fused into one pass over the frame:
 * for each pixel, computes diff = |current - previous|, bins it into one of 7 non-uniform
 * histogram buckets (or no bucket, if diff is below the first edge), and overwrites previous with
 * current (the "imgOld" update, done every frame regardless of the diff outcome). Must be called
 * on the raw, pre-rotation/pre-border frame, matching the old pipeline's stage ordering.
 * @param current Device pointer to the current frame (MONO8).
 * @param currentPitch Row stride, in bytes, of current.
 * @param previous Device pointer to the previous-frame buffer (MONO8, same dims as current) -
 * updated in place for the next call. Caller must zero-initialize it before the first call
 * (matches the old pipeline's zero-initialized imgOld on a worker's first frame).
 * @param previousPitch Row stride, in bytes, of previous.
 * @param width, height Frame dimensions, in pixels.
 * @param useThirtyEdgeTable Selects the {30,40,60,80,100,120,140,256} bin-edge table
 * (`--minBrightChange 30`) instead of the default {20,30,40,60,80,100,120,256} table.
 * @param histCounts Device pointer to 7 uint32_t bin counts. Zeroed by this call before
 * accumulating - caller does not need to clear it first.
 */
void launchMotionDiffKernel(const uint8_t* current, uint32_t currentPitch, uint8_t* previous,
                            uint32_t previousPitch, uint32_t width, uint32_t height, bool useThirtyEdgeTable,
                            uint32_t* histCounts);

/**
 * Nearest-neighbor downscale of a MONO8 device image (PROCESSING_SPEC_teeldyne.md §3.25's live
 * preview, which downscales the fully composited/overlaid frame before display - caller decides
 * the target size, see motiondetecttask.cpp's c_previewFinalScale) - not bilinear/area-averaged
 * like cv::resize's default, but adequate for an operator-facing preview and far simpler; revisit
 * if preview image quality turns out to matter.
 * @param src, srcPitch, srcWidth, srcHeight Source image.
 * @param dst, dstPitch, dstWidth, dstHeight Destination image (caller-allocated, any size).
 */
void launchDownscaleKernel(const uint8_t* src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight, uint8_t* dst,
                           uint32_t dstPitch, uint32_t dstWidth, uint32_t dstHeight);
