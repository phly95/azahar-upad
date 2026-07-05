// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>
#endif

namespace Vulkan {

class Instance;

class FrameStreamer {
public:
    FrameStreamer(const Instance& instance, u32 width, u32 height);
    ~FrameStreamer();

    FrameStreamer(const FrameStreamer&) = delete;
    FrameStreamer& operator=(const FrameStreamer&) = delete;

    bool IsActive() const {
        return active;
    }

    void Start(const std::string& target_ip, u16 target_port);
    void Stop();

    bool RecordBlit(vk::CommandBuffer cmdbuf, vk::Image source, u32 src_width, u32 src_height,
                    u32 src_offset_x = 0, u32 src_offset_y = 0);
    void PushFrame();

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    vk::Image GetImage() const {
        return frames[write_index].image;
    }

private:
    struct FrameResources {
        vk::Image image;
        vk::DeviceMemory memory;
        vk::ImageView image_view;
        int cached_fd = -1;
        u32 stride = 0;
    };

    void CreateFrameResources(FrameResources& frame);
    void DestroyFrameResources(FrameResources& frame);
    int ExportDmaBuf(FrameResources& frame);
    void InitGstPipeline(const std::string& target_ip, u16 target_port);
    void CleanupGstPipeline();

    const Instance& instance;
    u32 width = 0;
    u32 height = 0;
    bool active = false;

    static constexpr size_t NUM_FRAMES = 3;
    std::array<FrameResources, NUM_FRAMES> frames{};
    u32 write_index = 0;
    u64 drm_modifier = 0;

#ifdef HAVE_GSTREAMER
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstAllocator* allocator = nullptr;
#endif
};

} // namespace Vulkan
