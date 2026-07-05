// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_vulkan/vk_frame_streamer.h"

#include <unistd.h>
#include <drm/drm_fourcc.h>

#include "common/logging/log.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef HAVE_GSTREAMER
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#endif

namespace Vulkan {

FrameStreamer::FrameStreamer(const Instance& instance, u32 width, u32 height)
    : instance(instance), width(width), height(height) {
    if (instance.IsExternalMemoryDmaBufSupported() &&
        instance.IsExternalMemoryFdSupported() &&
        instance.IsDrmFormatModifierSupported()) {
        bool all_ok = true;
        for (size_t i = 0; i < NUM_FRAMES; ++i) {
            CreateFrameResources(frames[i]);
            if (!frames[i].image) {
                all_ok = false;
                break;
            }
        }
        if (!all_ok) {
            for (size_t i = 0; i < NUM_FRAMES; ++i) {
                DestroyFrameResources(frames[i]);
            }
        }
    } else {
        LOG_WARNING(Render_Vulkan,
                    "FrameStreamer: DMA-BUF extensions not available. "
                    "Streaming will be disabled.");
    }
}

FrameStreamer::~FrameStreamer() {
    Stop();
    for (size_t i = 0; i < NUM_FRAMES; ++i) {
        DestroyFrameResources(frames[i]);
    }
}

void FrameStreamer::Start(const std::string& target_ip, u16 target_port,
                         u32 bitrate_kbps, u32 qp) {
    if (!frames[0].image) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: No streaming texture available.");
        return;
    }

#ifdef HAVE_GSTREAMER
    if (active) {
        Stop();
    }
    InitGstPipeline(target_ip, target_port, bitrate_kbps, qp);
    active = (pipeline != nullptr);
    if (active) {
        LOG_INFO(Render_Vulkan, "FrameStreamer: Streaming to {}:{} at {} kbps",
                 target_ip, target_port, bitrate_kbps);
    }
#else
    LOG_WARNING(Render_Vulkan,
                "FrameStreamer: GStreamer not available. Cannot start streaming.");
#endif
}

void FrameStreamer::Stop() {
#ifdef HAVE_GSTREAMER
    CleanupGstPipeline();
#endif
    active = false;
}

void FrameStreamer::CreateFrameResources(FrameResources& frame) {
    vk::Device device = instance.GetDevice();

    vk::ExternalMemoryImageCreateInfo external_info{};
    external_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

    vk::ImageCreateInfo image_info{};
    image_info.pNext = &external_info;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eR8G8B8A8Unorm;
    image_info.extent = vk::Extent3D{width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eLinear;
    image_info.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;

    vk::Image local_image = device.createImage(image_info);

    vk::MemoryRequirements mem_reqs = device.getImageMemoryRequirements(local_image);
    vk::PhysicalDeviceMemoryProperties mem_props =
        instance.GetPhysicalDevice().getMemoryProperties();

    u32 memory_type_index = 0;
    bool found = false;
    for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            memory_type_index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Failed to find suitable memory type");
        device.destroyImage(local_image);
        return;
    }

    vk::ExportMemoryAllocateInfo export_alloc_info{};
    export_alloc_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

    vk::MemoryAllocateInfo alloc_info{};
    alloc_info.pNext = &export_alloc_info;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = memory_type_index;

    vk::DeviceMemory local_memory = device.allocateMemory(alloc_info);
    device.bindImageMemory(local_image, local_memory, 0);

    if (auto func = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            device.getProcAddr("vkGetImageDrmFormatModifierPropertiesEXT"))) {
        VkImageDrmFormatModifierPropertiesEXT props{};
        props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
        if (func(device, local_image, &props) == VK_SUCCESS) {
            drm_modifier = props.drmFormatModifier;
        }
    }

    vk::ImageSubresource subres{};
    subres.aspectMask = vk::ImageAspectFlagBits::eColor;
    vk::SubresourceLayout layout;
    device.getImageSubresourceLayout(local_image, &subres, &layout);
    frame.stride = static_cast<u32>(layout.rowPitch);

    vk::ImageViewCreateInfo view_info{};
    view_info.image = local_image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = vk::Format::eR8G8B8A8Unorm;
    view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    vk::ImageView local_view = device.createImageView(view_info);

    frame.image = local_image;
    frame.memory = local_memory;
    frame.image_view = local_view;

    LOG_INFO(Render_Vulkan,
             "FrameStreamer: Buffer created ({}x{}, modifier=0x{:x}, stride={})",
             width, height, drm_modifier, frame.stride);
}

void FrameStreamer::DestroyFrameResources(FrameResources& frame) {
    vk::Device device = instance.GetDevice();

    if (frame.cached_fd >= 0) {
        close(frame.cached_fd);
        frame.cached_fd = -1;
    }

    if (frame.image_view) {
        device.destroyImageView(frame.image_view);
        frame.image_view = VK_NULL_HANDLE;
    }
    if (frame.image) {
        device.destroyImage(frame.image);
        frame.image = VK_NULL_HANDLE;
    }
    if (frame.memory) {
        device.freeMemory(frame.memory);
        frame.memory = VK_NULL_HANDLE;
    }
}

int FrameStreamer::ExportDmaBuf(FrameResources& frame) {
    if (!frame.memory) {
        return -1;
    }

    if (frame.cached_fd >= 0) {
        return dup(frame.cached_fd);
    }

    vk::Device device = instance.GetDevice();
    auto func = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        device.getProcAddr("vkGetMemoryFdKHR"));
    if (!func) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: vkGetMemoryFdKHR not available.");
        return -1;
    }

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = frame.memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    if (func(device, &fd_info, &frame.cached_fd) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Failed to export DMA-BUF fd.");
        return -1;
    }

    return dup(frame.cached_fd);
}

bool FrameStreamer::RecordBlit(vk::CommandBuffer cmdbuf, vk::Image source, u32 src_width,
                               u32 src_height, u32 src_offset_x, u32 src_offset_y) {
    auto& frame = frames[write_index];
    if (!frame.image) {
        return false;
    }

    auto MakeBarrier = [](vk::Image img, vk::ImageLayout old_layout,
                          vk::ImageLayout new_layout, vk::AccessFlags src_access,
                          vk::AccessFlags dst_access) {
        vk::ImageMemoryBarrier barrier{};
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img;
        barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        return barrier;
    };

    auto src_barrier = MakeBarrier(
        source, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
        vk::AccessFlagBits::eTransferRead);
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput |
                               vk::PipelineStageFlagBits::eFragmentShader,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, src_barrier);

    auto dst_barrier = MakeBarrier(frame.image, vk::ImageLayout::eUndefined,
                                   vk::ImageLayout::eTransferDstOptimal,
                                   vk::AccessFlagBits::eNone, vk::AccessFlagBits::eTransferWrite);
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, dst_barrier);

    vk::ImageBlit blit{};
    blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.srcSubresource.mipLevel = 0;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = vk::Offset3D{static_cast<s32>(src_offset_x), static_cast<s32>(src_offset_y), 0};
    blit.srcOffsets[1] = vk::Offset3D{static_cast<s32>(src_offset_x + src_width),
                                       static_cast<s32>(src_offset_y + src_height), 1};
    blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.dstSubresource.mipLevel = 0;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1};
    cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, frame.image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

    auto src_restore = MakeBarrier(
        source, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eTransferRead,
        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead);

    auto final_barrier = MakeBarrier(frame.image, vk::ImageLayout::eTransferDstOptimal,
                                     vk::ImageLayout::eGeneral,
                                     vk::AccessFlagBits::eTransferWrite,
                                     vk::AccessFlagBits::eMemoryRead);

    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, {}, {},
                           {src_restore, final_barrier});

    return true;
}

void FrameStreamer::PushFrame() {
#ifdef HAVE_GSTREAMER
    if (!active || !appsrc || !allocator) {
        return;
    }

    const u32 push_index = write_index;
    write_index = (write_index + 1) % NUM_FRAMES;

    auto& frame = frames[push_index];

    const int fd = ExportDmaBuf(frame);
    if (fd < 0) {
        return;
    }

    GstMemory* mem =
        gst_dmabuf_allocator_alloc(allocator, fd, frame.stride * height);
    if (!mem) {
        close(fd);
        LOG_WARNING(Render_Vulkan, "FrameStreamer: Failed to allocate GstMemory.");
        return;
    }

    GstBuffer* buf = gst_buffer_new();
    if (!buf) {
        gst_memory_unref(mem);
        LOG_WARNING(Render_Vulkan, "FrameStreamer: Failed to allocate GstBuffer.");
        return;
    }
    gst_buffer_append_memory(buf, mem);

    GST_BUFFER_PTS(buf) = gst_element_get_current_running_time(pipeline);
    GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);
    GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);

    if (ret != GST_FLOW_OK) {
        LOG_WARNING(Render_Vulkan, "FrameStreamer: GStreamer push failed: {}", ret);
    }
#endif
}

#ifdef HAVE_GSTREAMER

void FrameStreamer::InitGstPipeline(const std::string& target_ip, u16 target_port,
                                   u32 bitrate_kbps, u32 qp) {
    if (!gst_is_initialized()) {
        const auto& gpu_device = Settings::values.streaming_gpu_device.GetValue();
        if (!gpu_device.empty()) {
            g_setenv("GST_VAAPI_DRM_DEVICE", gpu_device.c_str(), TRUE);
            LOG_INFO(Render_Vulkan, "FrameStreamer: Set GST_VAAPI_DRM_DEVICE={}", gpu_device);
        }
        gst_init(nullptr, nullptr);
    }

    const auto encoder = Settings::values.streaming_encoder.GetValue();

    std::string enc_desc;
    switch (encoder) {
    case Settings::StreamingEncoder::VAAPI:
        enc_desc = "vaapih264enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
        LOG_INFO(Render_Vulkan, "FrameStreamer: Using vaapih264enc (QP={})", qp);
        break;
    case Settings::StreamingEncoder::VAAPI_LowPower:
        enc_desc = "vah264lpenc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
        LOG_INFO(Render_Vulkan, "FrameStreamer: Using vah264lpenc (QP={})", qp);
        break;
    case Settings::StreamingEncoder::x264:
        enc_desc = "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" +
                   std::to_string(bitrate_kbps);
        LOG_INFO(Render_Vulkan, "FrameStreamer: Using x264enc at {} kbps", bitrate_kbps);
        break;
    case Settings::StreamingEncoder::OpenH264:
        enc_desc = "openh264enc complexity=low bitrate=" + std::to_string(bitrate_kbps);
        LOG_INFO(Render_Vulkan, "FrameStreamer: Using openh264enc at {} kbps", bitrate_kbps);
        break;
    case Settings::StreamingEncoder::Auto:
    default: {
        GstElement* test = gst_element_factory_make("vaapih264enc", nullptr);
        if (test) {
            gst_object_unref(test);
            enc_desc = "vaapih264enc rate-control=cqp init-qp=" + std::to_string(qp) + " qp-ip=1";
            LOG_INFO(Render_Vulkan, "FrameStreamer: Auto-selected vaapih264enc (QP={})", qp);
        } else {
            enc_desc = "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=" +
                       std::to_string(bitrate_kbps);
            LOG_INFO(Render_Vulkan,
                     "FrameStreamer: Auto-selected x264enc at {} kbps (vaapi unavailable)",
                     bitrate_kbps);
        }
        break;
    }
    }

    std::string pipeline_desc = "appsrc name=src is-live=true format=3 "
                                "! videoconvert ! " +
                                enc_desc +
                                " ! h264parse "
                                "! rtph264pay config-interval=1 pt=96 "
                                "! udpsink host=" +
                                target_ip + " port=" + std::to_string(target_port);

    LOG_INFO(Render_Vulkan, "FrameStreamer: Pipeline: {}", pipeline_desc);

    GError* error = nullptr;
    pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
    if (error) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Pipeline parse error: {}", error->message);
        g_error_free(error);
        pipeline = nullptr;
        return;
    }

    appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (!appsrc) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Could not find appsrc element.");
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return;
    }

    GstVideoInfo vinfo;
    gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, width, height);
    GstCaps* caps = gst_video_info_to_caps(&vinfo);
    g_object_set(appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    if (!allocator) {
        allocator = gst_dmabuf_allocator_new();
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Failed to start pipeline (ret={}).", ret);
        gst_object_unref(appsrc);
        appsrc = nullptr;
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return;
    }

    LOG_INFO(Render_Vulkan, "FrameStreamer: GStreamer pipeline started successfully (ret={}).", ret);
}

void FrameStreamer::CleanupGstPipeline() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (appsrc) {
        gst_object_unref(appsrc);
        appsrc = nullptr;
    }
    if (pipeline) {
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }
    if (allocator) {
        gst_object_unref(allocator);
        allocator = nullptr;
    }
}

#else

void FrameStreamer::InitGstPipeline(const std::string& /*target_ip*/, u16 /*target_port*/,
                                   u32 /*bitrate_kbps*/, u32 /*qp*/) {}
void FrameStreamer::CleanupGstPipeline() {}

#endif

} // namespace Vulkan
