#pragma once
#include <vulkan/vulkan.hpp>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// Throws std::runtime_error if the file is missing or not a whole number of 32-bit words.
std::vector<uint32_t> loadSpirv(const std::filesystem::path& path);

struct ShaderPair {
    vk::ShaderEXT vert;
    vk::ShaderEXT frag;
};

// Creates a linked vertex + fragment shader object pair. Throws std::runtime_error on failure.
ShaderPair createShaderPair(vk::Device device,
    const std::filesystem::path& vertSpv,
    const std::filesystem::path& fragSpv,
    std::span<const vk::DescriptorSetLayout> setLayouts,
    std::span<const vk::PushConstantRange> pushConstants = {});

void destroyShaderPair(vk::Device device, ShaderPair& pair);

inline void bindShaderPair(vk::CommandBuffer cmd, const ShaderPair& pair) {
    const vk::ShaderStageFlagBits stages[] = { vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eFragment };
    const vk::ShaderEXT shaders[] = { pair.vert, pair.frag };
    cmd.bindShadersEXT(2, stages, shaders);
}
