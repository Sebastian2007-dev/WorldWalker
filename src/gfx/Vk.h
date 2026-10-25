#pragma once
#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdio>
#include <cstdlib>

#define VK_CHECK(expr)                                                                       \
    do {                                                                                     \
        VkResult r_ = (expr);                                                                \
        if (r_ != VK_SUCCESS) {                                                              \
            std::fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", int(r_), __FILE__, __LINE__, #expr); \
            std::abort();                                                                    \
        }                                                                                    \
    } while (0)

namespace gfx {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mips = 1;
};

}  // namespace gfx
