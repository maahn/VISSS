#include "kernel.cuh"

#include "font8x14.h"

#include <eSdkPro/errors.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <string>

// One block per character, one thread per glyph pixel - avoids any division/modulo in the kernel
// body, since blockIdx.x already is the character index.
__global__ void drawTextKernel(uint8_t* outBuf, uint32_t pitch, uint32_t frameWidth, uint32_t frameHeight,
                               const char* text, uint32_t originX, uint32_t originY)
{
    const uint32_t charIdx = blockIdx.x;
    const uint32_t localX = threadIdx.x;
    const uint32_t localY = threadIdx.y;

    const uint32_t px = originX + (charIdx * c_fontCellWidth) + localX;
    const uint32_t py = originY + localY;
    if (px >= frameWidth || py >= frameHeight)
    {
        return;
    }

    const char c = text[charIdx];
    if (c < c_fontFirstChar || c > c_fontLastChar)
    {
        // Unsupported character (outside the generated glyph range) - draw nothing, leaving
        // whatever background/border color is already there.
        return;
    }

    const uint8_t rowBits = g_font8x14[c - c_fontFirstChar][localY];
    const bool on = (rowBits >> (7 - localX)) & 1;
    if (on)
    {
        outBuf[(py * pitch) + px] = 255;
    }
}

void launchDrawTextKernel(eSdkPro::Frame& outputFrame, const char* deviceText, uint32_t textLen, uint32_t originX,
                          uint32_t originY)
{
    if (textLen == 0)
    {
        return;
    }

    const dim3 threadsPerBlock(c_fontCellWidth, c_fontCellHeight);
    const dim3 blocksPerGrid(textLen);

    drawTextKernel<<<blocksPerGrid, threadsPerBlock>>>(outputFrame.GetDataPtr(), outputFrame.GetStride(),
                                                        outputFrame.GetWidth(), outputFrame.GetHeight(), deviceText,
                                                        originX, originY);

    cudaDeviceSynchronize();
    if (cudaError_t err = cudaGetLastError(); err != cudaSuccess)
    {
        throw eSdkPro::ESdkProException(
            eSdkPro::ErrorCode::General, "Error during kernel execution for launchDrawTextKernel: " +
                                              std::string(cudaGetErrorString(err)));
    }
}

// One thread per destination pixel. Destination is srcHeight wide, srcWidth tall (dims swap on a
// 90-degree rotation) - see kernel.cuh's launchRotate90CcwKernel doc for the coordinate mapping
// derivation (matches cv::rotate's ROTATE_90_COUNTERCLOCKWISE: transpose then flip vertically).
__global__ void rotate90CcwKernel(const uint8_t* srcBuf, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight,
                                  uint8_t* dstBuf, uint32_t dstPitch)
{
    const uint32_t dstX = (blockIdx.x * blockDim.x) + threadIdx.x; // [0, srcHeight)
    const uint32_t dstY = (blockIdx.y * blockDim.y) + threadIdx.y; // [0, srcWidth)
    if (dstX >= srcHeight || dstY >= srcWidth)
    {
        return;
    }

    const uint32_t srcRow = dstX;
    const uint32_t srcCol = srcWidth - 1 - dstY;
    dstBuf[(dstY * dstPitch) + dstX] = srcBuf[(srcRow * srcPitch) + srcCol];
}

void launchRotate90CcwKernel(const uint8_t* srcBuf, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight,
                             uint8_t* dstBuf, uint32_t dstPitch)
{
    const dim3 threadsPerBlock(16, 16);
    const dim3 blocksPerGrid((srcHeight + threadsPerBlock.x - 1) / threadsPerBlock.x,
                             (srcWidth + threadsPerBlock.y - 1) / threadsPerBlock.y);

    rotate90CcwKernel<<<blocksPerGrid, threadsPerBlock>>>(srcBuf, srcPitch, srcWidth, srcHeight, dstBuf, dstPitch);

    cudaDeviceSynchronize();
    if (cudaError_t err = cudaGetLastError(); err != cudaSuccess)
    {
        throw eSdkPro::ESdkProException(
            eSdkPro::ErrorCode::General, "Error during kernel execution for launchRotate90CcwKernel: " +
                                              std::string(cudaGetErrorString(err)));
    }
}

// The two hardcoded bin-edge tables from PROCESSING_SPEC_teeldyne.md §3.18 - "the exact,
// hardcoded values to preserve". 256 (not 255) as the last edge is deliberate: it's an exclusive
// upper bound, so the 7th bin covers diff in [120,256) i.e. up to and including 255.
__constant__ int g_binEdges20[8] = {20, 30, 40, 60, 80, 100, 120, 256};
__constant__ int g_binEdges30[8] = {30, 40, 60, 80, 100, 120, 140, 256};

// Grid-stride loop over all pixels; each block accumulates into a shared-memory partial
// histogram first, then merges into the global one with 7 atomicAdds per block instead of one
// per pixel - histCounts must already be zeroed by the caller (launchMotionDiffKernel does this).
__global__ void motionDiffKernel(const uint8_t* cur, uint32_t curPitch, uint8_t* prev, uint32_t prevPitch,
                                 uint32_t width, uint32_t height, bool useThirtyEdgeTable, uint32_t* histCounts)
{
    __shared__ uint32_t localHist[7];
    if (threadIdx.x < 7)
    {
        localHist[threadIdx.x] = 0;
    }
    __syncthreads();

    const int* edges = useThirtyEdgeTable ? g_binEdges30 : g_binEdges20;
    const uint32_t total = width * height;
    for (uint32_t i = (blockIdx.x * blockDim.x) + threadIdx.x; i < total; i += blockDim.x * gridDim.x)
    {
        const uint32_t y = i / width;
        const uint32_t x = i % width;
        uint8_t* prevPx = prev + (static_cast<size_t>(y) * prevPitch) + x;
        const uint8_t curVal = cur[(static_cast<size_t>(y) * curPitch) + x];
        const int diff = abs(static_cast<int>(curVal) - static_cast<int>(*prevPx));
        *prevPx = curVal; // imgOld update, every frame regardless of diff outcome

        if (diff >= edges[0])
        {
#pragma unroll
            for (int bin = 0; bin < 7; bin++)
            {
                if (diff >= edges[bin] && diff < edges[bin + 1])
                {
                    atomicAdd(&localHist[bin], 1u);
                    break;
                }
            }
        }
    }
    __syncthreads();

    if (threadIdx.x < 7 && localHist[threadIdx.x] > 0)
    {
        atomicAdd(&histCounts[threadIdx.x], localHist[threadIdx.x]);
    }
}

void launchMotionDiffKernel(const uint8_t* current, uint32_t currentPitch, uint8_t* previous,
                            uint32_t previousPitch, uint32_t width, uint32_t height, bool useThirtyEdgeTable,
                            uint32_t* histCounts)
{
    cudaError_t err = cudaMemset(histCounts, 0, 7 * sizeof(uint32_t));
    if (err != cudaSuccess)
    {
        throw eSdkPro::ESdkProException(
            eSdkPro::ErrorCode::General,
            "cudaMemset failed for motion histogram counts: " + std::string(cudaGetErrorString(err)));
    }

    const int threadsPerBlock = 256;
    const uint32_t totalPixels = width * height;
    const int blocksPerGrid =
        std::min(1024, static_cast<int>((totalPixels + threadsPerBlock - 1) / threadsPerBlock));

    motionDiffKernel<<<blocksPerGrid, threadsPerBlock>>>(current, currentPitch, previous, previousPitch, width,
                                                          height, useThirtyEdgeTable, histCounts);

    cudaDeviceSynchronize();
    if (cudaError_t launchErr = cudaGetLastError(); launchErr != cudaSuccess)
    {
        throw eSdkPro::ESdkProException(
            eSdkPro::ErrorCode::General, "Error during kernel execution for launchMotionDiffKernel: " +
                                              std::string(cudaGetErrorString(launchErr)));
    }
}

__global__ void downscaleKernel(const uint8_t* src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight,
                                uint8_t* dst, uint32_t dstPitch, uint32_t dstWidth, uint32_t dstHeight)
{
    const uint32_t dstX = (blockIdx.x * blockDim.x) + threadIdx.x;
    const uint32_t dstY = (blockIdx.y * blockDim.y) + threadIdx.y;
    if (dstX >= dstWidth || dstY >= dstHeight)
    {
        return;
    }

    const uint32_t srcX = (dstX * srcWidth) / dstWidth;
    const uint32_t srcY = (dstY * srcHeight) / dstHeight;
    dst[(dstY * dstPitch) + dstX] = src[(srcY * srcPitch) + srcX];
}

void launchDownscaleKernel(const uint8_t* src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight, uint8_t* dst,
                           uint32_t dstPitch, uint32_t dstWidth, uint32_t dstHeight)
{
    const dim3 threadsPerBlock(16, 16);
    const dim3 blocksPerGrid((dstWidth + threadsPerBlock.x - 1) / threadsPerBlock.x,
                             (dstHeight + threadsPerBlock.y - 1) / threadsPerBlock.y);

    downscaleKernel<<<blocksPerGrid, threadsPerBlock>>>(src, srcPitch, srcWidth, srcHeight, dst, dstPitch, dstWidth,
                                                        dstHeight);

    cudaDeviceSynchronize();
    if (cudaError_t err = cudaGetLastError(); err != cudaSuccess)
    {
        throw eSdkPro::ESdkProException(
            eSdkPro::ErrorCode::General,
            "Error during kernel execution for launchDownscaleKernel: " + std::string(cudaGetErrorString(err)));
    }
}
