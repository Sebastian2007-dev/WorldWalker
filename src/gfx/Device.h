#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "gfx/Vk.h"

struct GLFWwindow;

namespace gfx {

constexpr uint32_t kFramesInFlight = 2;

struct FrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t frameIndex = 0;  // 0..kFramesInFlight-1
    uint32_t imageIndex = 0;
    VkImage swapImage = VK_NULL_HANDLE;
    VkImageView swapView = VK_NULL_HANDLE;
};

class Device {
public:
    void init(GLFWwindow* window, bool validation);
    void shutdown();

    // Returns false if the swapchain had to be recreated (skip the frame).
    bool beginFrame(FrameContext& fc);
    void endFrame(FrameContext& fc);
    void waitIdle() { vkDeviceWaitIdle(device_); }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage mem,
                        bool mapped = false, bool deviceAddress = true);
    Image createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, uint32_t mips = 1,
                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    // Destruction is deferred until the GPU is guaranteed to be done with the resource.
    void destroyLater(Buffer& b);
    void destroyLater(Image& i);
    void destroyNow(Buffer& b);
    void destroyNow(Image& i);

    // Queue a host->device copy that is recorded at the start of the next frame (thread-safe).
    void uploadBuffer(const Buffer& dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);
    // Synchronous one-off submission (startup only).
    void immediate(const std::function<void(VkCommandBuffer)>& fn);
    // Upload RGBA8 pixels into a new mipmapped sampled image (synchronous).
    Image createTexture2D(const void* rgba, uint32_t w, uint32_t h, VkFormat format, bool mipmaps);

    VkShaderModule loadShader(const std::string& name);

    VkDevice device() const { return device_; }
    VkPhysicalDevice physical() const { return phys_; }
    VkInstance instance() const { return instance_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VmaAllocator allocator() const { return vma_; }
    VkFormat swapFormat() const { return swapFormat_; }
    VkExtent2D swapExtent() const { return swapExtent_; }
    uint32_t swapImageCount() const { return uint32_t(swapImages_.size()); }
    uint64_t frameNumber() const { return frameNumber_; }
    const VkPhysicalDeviceProperties& props() const { return props_; }

    bool resized = false;

private:
    void createSwapchain();
    void destroySwapchain();
    void flushUploads(VkCommandBuffer cmd);
    void collectGarbage(uint32_t frame);

    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VmaAllocator vma_ = nullptr;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D swapExtent_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderDone_;  // per swapchain image

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        std::vector<Buffer> deadBuffers;
        std::vector<Image> deadImages;
    } frames_[kFramesInFlight];
    uint32_t frame_ = 0;
    uint64_t frameNumber_ = 0;

    VkCommandPool immPool_ = VK_NULL_HANDLE;

    struct PendingUpload {
        Buffer staging;
        VkBuffer dst;
        VkDeviceSize offset, size;
    };
    std::mutex uploadMutex_;
    std::vector<PendingUpload> uploads_;
    std::mutex garbageMutex_;
};

}  // namespace gfx
