#include "gfx/Pipeline.h"
#include <algorithm>
#include <stdexcept>

namespace gfx {

void Bindless::init(Device& dev) {
    dev_ = &dev;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = kMaxBindlessTextures;
    b.stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                     VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                     VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &bf;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 1;
    lci.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev.device(), &lci, nullptr, &layout_));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(dev.device(), &pci, nullptr, &pool_));

    uint32_t count = kMaxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    vc.descriptorSetCount = 1;
    vc.pDescriptorCounts = &count;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.pNext = &vc;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout_;
    VK_CHECK(vkAllocateDescriptorSets(dev.device(), &ai, &set_));

    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, kPushConstantSize};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(dev.device(), &plci, nullptr, &pipelineLayout_));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = 16.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev.device(), &sci, nullptr, &linearRepeat_));
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(dev.device(), &sci, nullptr, &linearClamp_));
}

void Bindless::shutdown() {
    VkDevice d = dev_->device();
    vkDestroySampler(d, linearRepeat_, nullptr);
    vkDestroySampler(d, linearClamp_, nullptr);
    vkDestroyPipelineLayout(d, pipelineLayout_, nullptr);
    vkDestroyDescriptorPool(d, pool_, nullptr);
    vkDestroyDescriptorSetLayout(d, layout_, nullptr);
}

uint32_t Bindless::add(const Image& img, VkSampler sampler) {
    uint32_t idx = next_;
    auto reusable = std::find_if(released_.begin(), released_.end(),
                                 [&](auto& r) { return r.second + kFramesInFlight < dev_->frameNumber(); });
    if (reusable != released_.end()) {
        idx = reusable->first;
        released_.erase(reusable);
    } else {
        if (next_ >= kMaxBindlessTextures) throw std::runtime_error("bindless texture slots exhausted");
        ++next_;
    }
    VkDescriptorImageInfo ii{sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.dstArrayElement = idx;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_->device(), 1, &w, 0, nullptr);
    return idx;
}

void Bindless::release(uint32_t index) { released_.push_back({index, dev_->frameNumber()}); }

VkPipeline createGraphicsPipeline(Device& dev, VkPipelineLayout layout, const GraphicsPipelineDesc& d) {
    VkShaderModule vs = dev.loadShader(d.vs);
    VkShaderModule fs = d.fs.empty() ? VK_NULL_HANDLE : dev.loadShader(d.fs);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d.polygonMode;
    rs.cullMode = d.cull;
    rs.frontFace = d.frontFace;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest;
    ds.depthWriteEnable = d.depthWrite;
    ds.depthCompareOp = d.depthOp;
    std::vector<VkPipelineColorBlendAttachmentState> blends(d.colorFormats.size());
    for (auto& b : blends) {
        b.colorWriteMask = 0xF;
        if (d.alphaBlend) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.colorBlendOp = VK_BLEND_OP_ADD;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = uint32_t(blends.size());
    cb.pAttachments = blends.data();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = uint32_t(d.colorFormats.size());
    rci.pColorAttachmentFormats = d.colorFormats.data();
    rci.depthAttachmentFormat = d.depthFormat;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rci;
    pci.stageCount = fs ? 2 : 1;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dy;
    pci.layout = layout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(dev.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &p));
    vkDestroyShaderModule(dev.device(), vs, nullptr);
    if (fs) vkDestroyShaderModule(dev.device(), fs, nullptr);
    return p;
}

VkPipeline createComputePipeline(Device& dev, VkPipelineLayout layout, const std::string& cs) {
    VkShaderModule m = dev.loadShader(cs);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
    ci.layout = layout;
    VkPipeline p;
    VK_CHECK(vkCreateComputePipelines(dev.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    vkDestroyShaderModule(dev.device(), m, nullptr);
    return p;
}

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = img;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

}  // namespace gfx
