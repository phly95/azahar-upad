// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_vulkan/vk_frame_streamer.h"

#include <unistd.h>
#include <drm/drm_fourcc.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef HAVE_GSTREAMER
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#endif

namespace Vulkan {

FrameStreamer::FrameStreamer(const Instance& instance, u32 width, u32 height)
    : instance(instance), width(width), height(height) {
    if (instance.IsExternalMemoryDmaBufSupported() &&
        instance.IsExternalMemoryFdSupported() &&
        instance.IsDrmFormatModifierSupported()) {
        CreateStreamingTexture();
    } else {
        LOG_WARNING(Render_Vulkan,
                    "FrameStreamer: DMA-BUF extensions not available. "
                    "Streaming will be disabled.");
    }
}

FrameStreamer::~FrameStreamer() {
    Stop();
    DestroyStreamingTexture();
}

void FrameStreamer::Start(const std::string& target_ip, u16 target_port) {
    if (!streaming_image) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: No streaming texture available.");
        return;
    }

#ifdef HAVE_GSTREAMER
    if (active) {
        Stop();
    }
    InitGstPipeline(target_ip, target_port);
    active = (pipeline != nullptr);
    if (active) {
        LOG_INFO(Render_Vulkan, "FrameStreamer: Streaming to {}:{}", target_ip, target_port);
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

void FrameStreamer::CreateStreamingTexture() {
    vk::Device device = instance.GetDevice();

    vk::ExternalMemoryImageCreateInfo external_info{};
    external_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

    const vk::ImageCreateInfo image_info = {
        .pNext = &external_info,
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eLinear,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    streaming_image = device.createImage(image_info);

    vk::MemoryRequirements mem_reqs = device.getImageMemoryRequirements(streaming_image);
    vk::PhysicalDeviceMemoryProperties mem_props = instance.GetPhysicalDevice().getMemoryProperties();

    u32 memory_type_index = 0;
    bool found = false;
    for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            memory_type_index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Failed to find suitable memory type");
        return;
    }

    vk::ExportMemoryAllocateInfo export_alloc_info{};
    export_alloc_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

    vk::MemoryAllocateInfo alloc_info = {
        .pNext = &export_alloc_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = memory_type_index,
    };

    streaming_memory = device.allocateMemory(alloc_info);
    device.bindImageMemory(streaming_image, streaming_memory, 0);

    // Query the DRM format modifier
    if (auto func = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            device.getProcAddr("vkGetImageDrmFormatModifierPropertiesEXT"))) {
        VkImageDrmFormatModifierPropertiesEXT props{};
        props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
        if (func(device, streaming_image, &props) == VK_SUCCESS) {
            drm_modifier = props.drmFormatModifier;
        }
    }

    // Query the stride for DMA-BUF layout
    vk::ImageSubresource subres{vk::ImageAspectFlagBits::eColor, 0, 0};
    vk::SubresourceLayout layout;
    device.getImageSubresourceLayout(streaming_image, &subres, &layout);
    stride = static_cast<u32>(layout.rowPitch);

    // Create image view
    const vk::ImageViewCreateInfo view_info = {
        .image = streaming_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    streaming_image_view = device.createImageView(view_info);

    LOG_INFO(Render_Vulkan, "FrameStreamer: Streaming texture created ({}x{}, modifier=0x{:x}, "
                            "stride={})",
             width, height, drm_modifier, stride);
}

void FrameStreamer::DestroyStreamingTexture() {
    vk::Device device = instance.GetDevice();

    if (cached_fd >= 0) {
        close(cached_fd);
        cached_fd = -1;
    }

    if (streaming_image_view) {
        device.destroyImageView(streaming_image_view);
        streaming_image_view = VK_NULL_HANDLE;
    }
    if (streaming_image) {
        device.destroyImage(streaming_image);
        streaming_image = VK_NULL_HANDLE;
    }
    if (streaming_memory) {
        device.freeMemory(streaming_memory);
        streaming_memory = VK_NULL_HANDLE;
    }
}

int FrameStreamer::ExportDmaBuf() {
    if (!streaming_memory) {
        return -1;
    }

    if (cached_fd >= 0) {
        return dup(cached_fd);
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
    fd_info.memory = streaming_memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    if (func(device, &fd_info, &cached_fd) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Failed to export DMA-BUF fd.");
        return -1;
    }

    return dup(cached_fd);
}

bool FrameStreamer::RecordBlit(vk::CommandBuffer cmdbuf, vk::Image source,
                               u32 src_width, u32 src_height) {
    if (!streaming_image) {
        return false;
    }

    // Transition source to transfer src optimal
    const vk::ImageMemoryBarrier src_barrier = {
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = source,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput |
                               vk::PipelineStageFlagBits::eFragmentShader,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, src_barrier);

    // Transition streaming texture to transfer dst
    const vk::ImageMemoryBarrier dst_barrier = {
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = streaming_image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, dst_barrier);

    // Blit from source to streaming texture (handles format conversion on GPU)
    const vk::ImageBlit blit = {
        .srcSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = {{{0, 0, 0}, {static_cast<i32>(src_width), static_cast<i32>(src_height), 1}}},
        .dstSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = {{{0, 0, 0}, {static_cast<i32>(width), static_cast<i32>(height), 1}}},
    };
    cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, streaming_image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

    // Restore source image to general
    const vk::ImageMemoryBarrier src_restore = {
        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = source,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    // Transition streaming texture to general (needed for DMA-BUF export)
    const vk::ImageMemoryBarrier final_barrier = {
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = streaming_image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
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

    const int fd = ExportDmaBuf();
    if (fd < 0) {
        return;
    }

    // Create GstBuffer wrapping the DMA-BUF fd
    GstMemory* mem = gst_fd_allocator_alloc(allocator, fd, stride * height,
                                            GST_FD_MEMORY_FLAG_NONE);
    GstBuffer* buf = gst_buffer_new();
    gst_buffer_append_memory(buf, mem);

    static guint64 frame_count = 0;
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(frame_count, GST_SECOND, 30);
    GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale_int(1, GST_SECOND, 30); // 30fps
    frame_count++;

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);

    if (ret != GST_FLOW_OK) {
        LOG_WARNING(Render_Vulkan, "FrameStreamer: GStreamer push failed: {}", ret);
    }
#endif
}

#ifdef HAVE_GSTREAMER



void FrameStreamer::InitGstPipeline(const std::string& target_ip, u16 target_port) {
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }

    // Build pipeline: appsrc → videoconvert → encoder → h264parse → rtp → udpsink
    // Try VA-API hardware encoder first, fall back to software x264enc
    std::string enc_desc;
    GstElement* test_encoder = gst_element_factory_make("vaapih264enc", nullptr);
    if (test_encoder) {
        gst_object_unref(test_encoder);
        enc_desc = "vaapih264enc rate-control=cqp quant-i=22 quant-p=23";
    } else {
        enc_desc = "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=400";
    }

    std::string pipeline_desc = "appsrc name=src is-live=true format=3 "
                                "! videoconvert ! " +
                                enc_desc +
                                " ! h264parse "
                                "! rtph264pay config-interval=1 pt=96 "
                                "! udpsink host=" +
                                target_ip + " port=" + std::to_string(target_port);

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

    // Configure appsrc caps
    GstVideoInfo vinfo;
    gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, width, height);
    vinfo.fps_n = 30;
    vinfo.fps_d = 1;
    GstCaps* caps = gst_video_info_to_caps(&vinfo);
    gst_caps_set_features(caps, 0, gst_caps_features_new("memory:DMABuf", NULL));
    g_object_set(appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    if (!allocator) {
        allocator = gst_fd_allocator_new();
    }

    // Start the pipeline
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR(Render_Vulkan, "FrameStreamer: Failed to start pipeline.");
        gst_object_unref(appsrc);
        appsrc = nullptr;
        gst_object_unref(pipeline);
        pipeline = nullptr;
        return;
    }

    LOG_INFO(Render_Vulkan, "FrameStreamer: GStreamer pipeline started successfully.");
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

void FrameStreamer::InitGstPipeline(const std::string& /*target_ip*/, u16 /*target_port*/) {}
void FrameStreamer::CleanupGstPipeline() {}

#endif

} // namespace Vulkan
