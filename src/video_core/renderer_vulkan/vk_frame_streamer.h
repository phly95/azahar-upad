// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vulkan/vulkan.hpp>

#include "common/common_types.h"

#include <vk_mem_alloc.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstfdmemory.h>
#endif

namespace Vulkan {

class Instance;

/// Manages a DMA-BUF exportable texture and an optional GStreamer pipeline
/// for zero-copy network streaming of rendered frames.
class FrameStreamer {
public:
    FrameStreamer(const Instance& instance, u32 width, u32 height);
    ~FrameStreamer();

    FrameStreamer(const FrameStreamer&) = delete;
    FrameStreamer& operator=(const FrameStreamer&) = delete;

    /// Returns true when streaming is active and a pipeline is running.
    bool IsActive() const {
        return active;
    }

    /// Starts the GStreamer pipeline targeting the given host and port.
    void Start(const std::string& target_ip, u16 target_port);

    /// Stops the GStreamer pipeline and releases resources.
    void Stop();

    /// Records a GPU-side blit from @p source into the streaming texture.
    /// Must be called while recording into the active command buffer.
    /// The source image must have been transitioned to eTransferSrcOptimal or eGeneral.
    bool RecordBlit(vk::CommandBuffer cmdbuf, vk::Image source, u32 src_width, u32 src_height);

    /// Exports the streaming texture's DMA-BUF fd and pushes a frame to GStreamer.
    /// Must be called after the command buffer containing the blit has completed.
    void PushFrame();

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    vk::Image GetImage() const {
        return streaming_image;
    }

private:
    void CreateStreamingTexture();
    void DestroyStreamingTexture();
    int ExportDmaBuf();
    void InitGstPipeline(const std::string& target_ip, u16 target_port);
    void CleanupGstPipeline();

    const Instance& instance;
    u32 width = 0;
    u32 height = 0;
    bool active = false;

    // Exportable streaming texture (RGBA8, linear tiling)
    vk::Image streaming_image;
    vk::DeviceMemory streaming_memory;
    vk::ImageView streaming_image_view;
    u64 drm_modifier = 0;
    u32 stride = 0;
    int cached_fd = -1;

#ifdef HAVE_GSTREAMER
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstAllocator* allocator = nullptr;
#endif
};

} // namespace Vulkan
