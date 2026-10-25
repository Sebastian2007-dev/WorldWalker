#pragma once
#include <string>
#include <vector>

#include "gfx/Device.h"

namespace gfx {

constexpr uint32_t kMaxBindlessTextures = 4096;
constexpr uint32_t kPushConstantSize = 128;

// One descriptor set for the whole renderer: a bindless array of combined image samplers.
// Everything else (buffers) is accessed through buffer device addresses in push constants.
class Bindless {
public:
    void init(Device& dev);
    void shutdown();
    uint32_t add(const Image& img, VkSampler sampler);
    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet set() const { return set_; }
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkSampler linearRepeat() const { return linearRepeat_; }
    VkSampler linearClamp() const { return linearClamp_; }

private:
    Device* dev_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkSampler linearRepeat_ = VK_NULL_HANDLE, linearClamp_ = VK_NULL_HANDLE;
    uint32_t next_ = 0;
};

struct GraphicsPipelineDesc {
    std::string vs, fs;
    VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthTest = true, depthWrite = true;
    VkCompareOp depthOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed Z
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool alphaBlend = false;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
};

VkPipeline createGraphicsPipeline(Device& dev, VkPipelineLayout layout, const GraphicsPipelineDesc& d);
VkPipeline createComputePipeline(Device& dev, VkPipelineLayout layout, const std::string& cs);

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

}  // namespace gfx
