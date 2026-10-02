#pragma once
#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <volk.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <string>
#include <vector>
#include "Vertex.h"

// GPU-only (VRAM) buffer. Contents are filled through a staging upload.
class DeviceBuffer {
public:
    DeviceBuffer(VmaAllocator allocator, vk::DeviceSize size, vk::BufferUsageFlags usage)
        : m_allocator(allocator), m_size(size)
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = static_cast<VkBufferUsageFlags>(usage | vk::BufferUsageFlagBits::eTransferDst);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkBuffer rawBuffer;
        if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &rawBuffer, &m_allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create device buffer");
        }
        m_buffer = vk::Buffer(rawBuffer);
    }

    ~DeviceBuffer() {
        if (m_buffer && m_allocation)
            vmaDestroyBuffer(m_allocator, VkBuffer(m_buffer), m_allocation);
    }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    vk::Buffer getBuffer() const { return m_buffer; }
    vk::DeviceSize getSize() const { return m_size; }

private:
    VmaAllocator m_allocator;
    vk::Buffer m_buffer;
    VmaAllocation m_allocation = nullptr;
    vk::DeviceSize m_size;
};

// Persistently mapped, CPU-written buffer that grows on demand. Only reserve() it once the GPU
// is no longer using it (e.g. after waiting on the owning frame's fence).
class HostBuffer {
public:
    HostBuffer(VmaAllocator allocator, vk::BufferUsageFlags usage, vk::DeviceSize initialSize)
        : m_allocator(allocator), m_usage(usage) {
        create(initialSize);
    }

    ~HostBuffer() { destroy(); }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    // Returns true if the buffer was recreated (descriptors pointing at it must be rewritten).
    bool reserve(vk::DeviceSize size) {
        if (size <= m_capacity) return false;
        destroy();
        create(std::max(size, m_capacity * 2));
        return true;
    }

    void* mapped() const { return m_mapped; }
    vk::Buffer getBuffer() const { return m_buffer; }
    vk::DeviceSize capacity() const { return m_capacity; }

private:
    void create(vk::DeviceSize size) {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = static_cast<VkBufferUsageFlags>(m_usage);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer rawBuffer;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &rawBuffer, &m_allocation, &info) != VK_SUCCESS)
            throw std::runtime_error("Failed to create host buffer");
        m_buffer = vk::Buffer(rawBuffer);
        m_mapped = info.pMappedData;
        m_capacity = size;
    }

    void destroy() {
        if (m_buffer) vmaDestroyBuffer(m_allocator, VkBuffer(m_buffer), m_allocation);
        m_buffer = nullptr;
        m_allocation = nullptr;
        m_mapped = nullptr;
    }

    VmaAllocator m_allocator;
    vk::BufferUsageFlags m_usage;
    vk::Buffer m_buffer;
    VmaAllocation m_allocation = nullptr;
    void* m_mapped = nullptr;
    vk::DeviceSize m_capacity = 0;
};

struct UBOBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
};
