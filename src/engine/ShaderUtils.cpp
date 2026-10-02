#include "ShaderUtils.h"
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

std::vector<uint32_t> loadSpirv(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Failed to open SPIR-V file");

    size_t size = file.tellg();
    if (size % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V size");

    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

ShaderPair createShaderPair(vk::Device device,
    const std::filesystem::path& vertSpv,
    const std::filesystem::path& fragSpv,
    std::span<const vk::DescriptorSetLayout> setLayouts,
    std::span<const vk::PushConstantRange> pushConstants)
{
    const std::vector<uint32_t> vertCode = loadSpirv(vertSpv);
    const std::vector<uint32_t> fragCode = loadSpirv(fragSpv);

    auto makeInfo = [&](vk::ShaderStageFlagBits stage, const std::vector<uint32_t>& code) {
        vk::ShaderCreateInfoEXT info{};
        info.setStage(stage)
            .setFlags(vk::ShaderCreateFlagBitsEXT::eLinkStage)
            .setCodeType(vk::ShaderCodeTypeEXT::eSpirv)
            .setCodeSize(code.size() * sizeof(uint32_t))
            .setPCode(code.data())
            .setPName("main")
            .setSetLayoutCount(static_cast<uint32_t>(setLayouts.size()))
            .setPSetLayouts(setLayouts.data())
            .setPushConstantRangeCount(static_cast<uint32_t>(pushConstants.size()))
            .setPPushConstantRanges(pushConstants.data());
        return info;
    };

    std::array<vk::ShaderCreateInfoEXT, 2> infos = {
        makeInfo(vk::ShaderStageFlagBits::eVertex, vertCode),
        makeInfo(vk::ShaderStageFlagBits::eFragment, fragCode),
    };
    infos[0].setNextStage(vk::ShaderStageFlagBits::eFragment);

    // Linking only applies to shaders created in the same vkCreateShadersEXT call.
    auto created = device.createShadersEXT(infos);
    if (created.result != vk::Result::eSuccess) {
        for (vk::ShaderEXT shader : created.value)
            if (shader) device.destroyShaderEXT(shader);
        throw std::runtime_error("Failed to create shader objects for " + vertSpv.string() + " / " +
            fragSpv.string() + ": " + vk::to_string(created.result));
    }
    return { created.value[0], created.value[1] };
}

void destroyShaderPair(vk::Device device, ShaderPair& pair)
{
    if (pair.vert) device.destroyShaderEXT(pair.vert);
    if (pair.frag) device.destroyShaderEXT(pair.frag);
    pair = {};
}
