#include "ModelManager.h"
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <tuple>
#include "stb_image.h"
#include "Shadow.h"
#include "IfcScene.h"
#include "ModelLoader.h"
#include "Log.h"

class TextureImage {
public:
    TextureImage(VmaAllocator allocator, vk::Device device, uint32_t width, uint32_t height,
        vk::Format format, uint32_t mipLevels)
        : m_allocator(allocator), m_device(device), m_width(width), m_height(height), m_mipLevels(mipLevels) {

        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo imageAllocInfo = {};
        imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkImage rawImage;
        if (vmaCreateImage(allocator, &imageInfo, &imageAllocInfo, &rawImage, &m_allocation, nullptr) != VK_SUCCESS)
            throw std::runtime_error("Failed to create texture image");
        m_image = vk::Image(rawImage);

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = m_image;
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = 1;
        m_view = device.createImageView(viewInfo).value;
    }

    ~TextureImage() {
        if (m_view) m_device.destroyImageView(m_view);
        if (m_image) vmaDestroyImage(m_allocator, VkImage(m_image), m_allocation);
    }

    TextureImage(const TextureImage&) = delete;
    TextureImage& operator=(const TextureImage&) = delete;

    vk::Image getImage() const { return m_image; }
    vk::ImageView getView() const { return m_view; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t mipLevels() const { return m_mipLevels; }

private:
    VmaAllocator m_allocator;
    vk::Device m_device;
    vk::Image m_image;
    VmaAllocation m_allocation = nullptr;
    vk::ImageView m_view;
    uint32_t m_width, m_height, m_mipLevels;
};

namespace {

// Records every copy for one model into a single command buffer backed by one staging
// buffer, then submits once and waits on a fence (instead of a queue.waitIdle per resource).
class UploadBatch {
public:
    UploadBatch(VmaAllocator allocator, vk::Device device, vk::CommandPool pool, vk::Queue queue, vk::DeviceSize stagingSize)
        : m_allocator(allocator), m_device(device), m_pool(pool), m_queue(queue) {
        VkBufferCreateInfo stagingInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = std::max<vk::DeviceSize>(stagingSize, 16);
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAllocInfo = {};
        stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &stagingInfo, &stagingAllocInfo, &m_staging, &m_stagingAlloc, &info) != VK_SUCCESS)
            throw std::runtime_error("Failed to create staging buffer");
        m_mapped = static_cast<char*>(info.pMappedData);

        vk::CommandBufferAllocateInfo allocInfo(pool, vk::CommandBufferLevel::ePrimary, 1);
        m_cmd = device.allocateCommandBuffers(allocInfo).value[0];
        (void)m_cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    }

    ~UploadBatch() {
        m_device.freeCommandBuffers(m_pool, m_cmd);
        vmaDestroyBuffer(m_allocator, m_staging, m_stagingAlloc);
    }

    static vk::DeviceSize alignUp(vk::DeviceSize v) { return (v + 15) & ~vk::DeviceSize(15); }

    void copyToBuffer(const DeviceBuffer& dst, const void* data, vk::DeviceSize size) {
        vk::DeviceSize offset = stage(data, size);
        vk::BufferCopy region(offset, 0, size);
        m_cmd.copyBuffer(vk::Buffer(m_staging), dst.getBuffer(), 1, &region);
    }

    void copyToImage(const TextureImage& img, const void* pixels, bool generateMips) {
        const vk::DeviceSize size = vk::DeviceSize(img.width()) * img.height() * 4;
        vk::DeviceSize offset = stage(pixels, size);
        const uint32_t levels = img.mipLevels();

        imageBarrier(img.getImage(), 0, levels,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);

        vk::BufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ img.width(), img.height(), 1 };
        m_cmd.copyBufferToImage(vk::Buffer(m_staging), img.getImage(), vk::ImageLayout::eTransferDstOptimal, 1, &region);

        int32_t w = static_cast<int32_t>(img.width());
        int32_t h = static_cast<int32_t>(img.height());
        for (uint32_t level = 1; generateMips && level < levels; ++level) {
            imageBarrier(img.getImage(), level - 1, 1,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);

            int32_t nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
            vk::ImageBlit blit{};
            blit.srcSubresource = { vk::ImageAspectFlagBits::eColor, level - 1, 0, 1 };
            blit.srcOffsets[1] = vk::Offset3D{ w, h, 1 };
            blit.dstSubresource = { vk::ImageAspectFlagBits::eColor, level, 0, 1 };
            blit.dstOffsets[1] = vk::Offset3D{ nw, nh, 1 };
            m_cmd.blitImage(img.getImage(), vk::ImageLayout::eTransferSrcOptimal,
                img.getImage(), vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);

            imageBarrier(img.getImage(), level - 1, 1,
                vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
            w = nw; h = nh;
        }

        const uint32_t lastLevel = generateMips ? levels - 1 : 0;
        imageBarrier(img.getImage(), lastLevel, levels - lastLevel,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
    }

    void submitAndWait() {
        vk::MemoryBarrier2 barrier{};
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput | vk::PipelineStageFlagBits2::eIndexInput;
        barrier.dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eIndexRead;
        vk::DependencyInfo dep{};
        dep.setMemoryBarriers(barrier);
        m_cmd.pipelineBarrier2(dep);
        (void)m_cmd.end();

        vk::Fence fence = m_device.createFence({}).value;
        vk::SubmitInfo submitInfo{};
        submitInfo.setCommandBuffers(m_cmd);
        (void)m_queue.submit(submitInfo, fence);
        (void)m_device.waitForFences(fence, VK_TRUE, UINT64_MAX);
        m_device.destroyFence(fence);
    }

private:
    vk::DeviceSize stage(const void* data, vk::DeviceSize size) {
        vk::DeviceSize offset = m_cursor;
        memcpy(m_mapped + offset, data, static_cast<size_t>(size));
        m_cursor = alignUp(offset + size);
        return offset;
    }

    void imageBarrier(vk::Image image, uint32_t baseMip, uint32_t mipCount,
        vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
        vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
        vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
        vk::ImageMemoryBarrier2 b{};
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = oldLayout; b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { vk::ImageAspectFlagBits::eColor, baseMip, mipCount, 0, 1 };
        vk::DependencyInfo dep{};
        dep.setImageMemoryBarriers(b);
        m_cmd.pipelineBarrier2(dep);
    }

    VmaAllocator m_allocator;
    vk::Device m_device;
    vk::CommandPool m_pool;
    vk::Queue m_queue;
    VkBuffer m_staging = VK_NULL_HANDLE;
    VmaAllocation m_stagingAlloc = nullptr;
    char* m_mapped = nullptr;
    vk::DeviceSize m_cursor = 0;
    vk::CommandBuffer m_cmd;
};

uint32_t mipLevelCount(uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    while ((w | h) >> levels) ++levels;
    return levels;
}

bool supportsLinearBlit(vk::PhysicalDevice physicalDevice, vk::Format format) {
    auto features = physicalDevice.getFormatProperties(format).optimalTilingFeatures;
    return (features & vk::FormatFeatureFlagBits::eBlitSrc) &&
        (features & vk::FormatFeatureFlagBits::eBlitDst) &&
        (features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
}

} // namespace

void TextureData::free() {
    if (pixels) {
        if (fromCache) {
            ::free(pixels);
        }
        else {
            stbi_image_free(pixels);
        }
        pixels = nullptr;
    }
}

ModelManager::ModelManager(VmaAllocator allocator, vk::Device device, vk::CommandPool cmdPool,
    vk::Queue queue, vk::DescriptorPool descriptorPool,
    vk::DescriptorSetLayout textureSetLayout, vk::Sampler textureSampler)
    : m_allocator(allocator)
    , m_device(device)
    , m_cmdPool(cmdPool)
    , m_queue(queue)
    , m_descriptorPool(descriptorPool)
    , m_textureSetLayout(textureSetLayout)
    , m_textureSampler(textureSampler)
{
    VmaAllocatorInfo allocatorInfo{};
    vmaGetAllocatorInfo(allocator, &allocatorInfo);
    vk::PhysicalDevice physicalDevice(allocatorInfo.physicalDevice);
    m_canGenerateMipsSrgb = supportsLinearBlit(physicalDevice, vk::Format::eR8G8B8A8Srgb);
    m_canGenerateMipsUnorm = supportsLinearBlit(physicalDevice, vk::Format::eR8G8B8A8Unorm);

    createDefaultTextures();
}

ModelManager::~ModelManager() {
    (void)m_device.waitIdle();
    m_models.clear();
    m_instances.clear();
}

void ModelManager::createDefaultTextures() {
    static const unsigned char whitePixels[] = { 255, 255, 255, 255 };
    static const unsigned char normalPixels[] = { 128, 128, 255, 255 };
    static const unsigned char mrPixels[] = { 0, 128, 0, 255 };

    m_defaultBaseColor = std::make_unique<TextureImage>(m_allocator, m_device, 1, 1, vk::Format::eR8G8B8A8Srgb, 1);
    m_defaultNormal = std::make_unique<TextureImage>(m_allocator, m_device, 1, 1, vk::Format::eR8G8B8A8Unorm, 1);
    m_defaultMR = std::make_unique<TextureImage>(m_allocator, m_device, 1, 1, vk::Format::eR8G8B8A8Unorm, 1);

    UploadBatch batch(m_allocator, m_device, m_cmdPool, m_queue, 3 * 16);
    batch.copyToImage(*m_defaultBaseColor, whitePixels, false);
    batch.copyToImage(*m_defaultNormal, normalPixels, false);
    batch.copyToImage(*m_defaultMR, mrPixels, false);
    batch.submitAndWait();

    m_defaultBaseColorSet = allocateTextureDescriptorSet(m_defaultBaseColor->getView());
    m_defaultNormalSet = allocateTextureDescriptorSet(m_defaultNormal->getView());
    m_defaultMRSet = allocateTextureDescriptorSet(m_defaultMR->getView());
}

vk::DescriptorSet ModelManager::allocateTextureDescriptorSet(vk::ImageView view) {
    vk::DescriptorSetAllocateInfo allocInfo{};
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_textureSetLayout;

    vk::DescriptorSet set = m_device.allocateDescriptorSets(allocInfo).value[0];

    vk::DescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    imageInfo.imageView = view;
    imageInfo.sampler = m_textureSampler;

    vk::WriteDescriptorSet write{};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    m_device.updateDescriptorSets(1, &write, 0, nullptr);

    return set;
}

void ModelManager::loadModelAsync(const std::string& path, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);

    LoadingTask task;
    task.path = path;
    task.name = !name.empty() ? name
        : path == kBuiltinCubePath ? std::string("Cube") : std::filesystem::path(path).stem().string();
    task.state = LoadingState::LoadingCPU;

    task.meshFuture = std::async(std::launch::async, [path]() {
        return loadModelSmart(path);
        });

    m_loadingTasks.push_back(std::move(task));
}

size_t ModelManager::loadModelSync(const std::string& path, const std::string& name) {
    std::string modelName = name.empty() ? std::filesystem::path(path).stem().string() : name;

    LOG_INFO("[ModelManager] Loading model synchronously: " << path << "\n");

    Mesh mesh = loadModelSmart(path);
    return uploadModelToGPU(mesh, modelName, path);
}

size_t ModelManager::uploadModelToGPU(Mesh& mesh, const std::string& name, const std::string& path) {
    auto gpuModel = std::make_unique<GPUModel>();
    gpuModel->name = name;
    gpuModel->sourcePath = path;
    gpuModel->submeshes = mesh.submeshes;
    gpuModel->drawOrder.resize(mesh.submeshes.size());
    std::iota(gpuModel->drawOrder.begin(), gpuModel->drawOrder.end(), 0u);
    std::stable_sort(gpuModel->drawOrder.begin(), gpuModel->drawOrder.end(), [&](uint32_t a, uint32_t b) {
        const Material& ma = mesh.submeshes[a].material;
        const Material& mb = mesh.submeshes[b].material;
        return std::tie(ma.baseColorTextureIndex, ma.normalTextureIndex, ma.metallicRoughnessTextureIndex) <
            std::tie(mb.baseColorTextureIndex, mb.normalTextureIndex, mb.metallicRoughnessTextureIndex);
        });
    gpuModel->vertexCount = mesh.vertices.size();
    gpuModel->indexCount = mesh.indices.size();
    gpuModel->ifcScene = mesh.ifcScene;
    gpuModel->positions.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices)
        gpuModel->positions.push_back(v.position);
    gpuModel->indices = mesh.indices;

    const vk::DeviceSize vertexBytes = sizeof(Vertex) * std::max<size_t>(mesh.vertices.size(), 1);
    const vk::DeviceSize indexBytes = sizeof(uint32_t) * std::max<size_t>(mesh.indices.size(), 1);

    vk::DeviceSize stagingSize = UploadBatch::alignUp(vertexBytes) + UploadBatch::alignUp(indexBytes);
    for (const auto& texData : mesh.textureData)
        stagingSize += UploadBatch::alignUp(vk::DeviceSize(texData.width) * texData.height * 4);

    gpuModel->vertexBuffer = std::make_unique<DeviceBuffer>(m_allocator, vertexBytes, vk::BufferUsageFlagBits::eVertexBuffer);
    gpuModel->indexBuffer = std::make_unique<DeviceBuffer>(m_allocator, indexBytes, vk::BufferUsageFlagBits::eIndexBuffer);

    {
        UploadBatch batch(m_allocator, m_device, m_cmdPool, m_queue, stagingSize);
        if (!mesh.vertices.empty())
            batch.copyToBuffer(*gpuModel->vertexBuffer, mesh.vertices.data(), sizeof(Vertex) * mesh.vertices.size());
        if (!mesh.indices.empty())
            batch.copyToBuffer(*gpuModel->indexBuffer, mesh.indices.data(), sizeof(uint32_t) * mesh.indices.size());

        for (auto& texData : mesh.textureData) {
            vk::Format fmt = texData.isLinear ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
            bool canMip = texData.isLinear ? m_canGenerateMipsUnorm : m_canGenerateMipsSrgb;
            uint32_t w = static_cast<uint32_t>(texData.width), h = static_cast<uint32_t>(texData.height);
            uint32_t levels = canMip ? mipLevelCount(w, h) : 1;

            auto tex = std::make_unique<TextureImage>(m_allocator, m_device, w, h, fmt, levels);
            batch.copyToImage(*tex, texData.pixels, canMip);
            gpuModel->textures.push_back(std::move(tex));
        }

        batch.submitAndWait();
    }

    for (auto& tex : gpuModel->textures)
        gpuModel->textureDescriptorSets.push_back(allocateTextureDescriptorSet(tex->getView()));
    for (auto& texData : mesh.textureData)
        texData.free();

    if (!mesh.vertices.empty()) {
        glm::vec3 bmin(FLT_MAX);
        glm::vec3 bmax(-FLT_MAX);

        for (const auto& v : mesh.vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }

        gpuModel->boundsMin = bmin;
        gpuModel->boundsMax = bmax;
        gpuModel->boundsCenter = (bmin + bmax) * 0.5f;
        gpuModel->boundsRadius = glm::length(bmax - bmin) * 0.5f;
    }

    size_t index = m_models.size();
    m_models.push_back(std::move(gpuModel));

    LOG_INFO("[ModelManager] Model '" << name << "' loaded: "
        << mesh.vertices.size() << " verts, "
        << mesh.indices.size() << " indices, "
        << m_models.back()->textures.size() << " textures\n");

    return index;
}

void ModelManager::unloadModel(size_t modelIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (modelIndex >= m_models.size()) return;

    // Remove instances using this model
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(),
            [modelIndex](const ModelInstance& inst) { return inst.modelIndex == modelIndex; }),
        m_instances.end());

    // Update indices
    for (auto& inst : m_instances) {
        if (inst.modelIndex > modelIndex) inst.modelIndex--;
    }

    (void)m_device.waitIdle();
    m_models.erase(m_models.begin() + modelIndex);
}

size_t ModelManager::createInstance(size_t modelIndex, const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (modelIndex >= m_models.size()) {
        throw std::runtime_error("Invalid model index");
    }

    ModelInstance inst;
    inst.modelIndex = modelIndex;
    inst.position = position;
    inst.rotation = rotation;
    inst.scale = scale;
    inst.name = m_models[modelIndex]->name + "_" + std::to_string(m_nextInstanceId++);
    inst.ifcScene = m_models[modelIndex]->ifcScene;

    m_instances.push_back(inst);
    return m_instances.size() - 1;
}

void ModelManager::removeInstance(size_t instanceIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (instanceIndex < m_instances.size()) {
        m_instances.erase(m_instances.begin() + instanceIndex);
    }
}

ModelInstance* ModelManager::getInstance(size_t instanceIndex) {
    if (instanceIndex < m_instances.size()) {
        return &m_instances[instanceIndex];
    }
    return nullptr;
}

GPUModel* ModelManager::getModel(size_t index) {
    if (index < m_models.size()) {
        return m_models[index].get();
    }
    return nullptr;
}

std::optional<size_t> ModelManager::findModelByPath(const std::string& path) const {
    for (size_t i = 0; i < m_models.size(); ++i) {
        if (m_models[i] && m_models[i]->isValid() && m_models[i]->sourcePath == path)
            return i;
    }
    return std::nullopt;
}

void ModelManager::update() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_loadingTasks.begin(); it != m_loadingTasks.end(); ) {
        LoadingTask& task = *it;

        if (task.state == LoadingState::LoadingCPU) {
            if (task.meshFuture.valid() &&
                task.meshFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                try {
                    task.loadedMesh = task.meshFuture.get();
                    task.state = LoadingState::UploadingGPU;
                }
                catch (const std::exception& e) {
                    task.state = LoadingState::Failed;
                    task.errorMessage = e.what();
                    LOG_ERROR("[ModelManager] Failed: " << e.what() << "\n");
                }
            }
        }

        if (task.state == LoadingState::UploadingGPU) {
            try {
                m_mutex.unlock();
                uploadModelToGPU(task.loadedMesh, task.name, task.path);
                m_mutex.lock();
                task.state = LoadingState::Complete;
            }
            catch (const std::exception& e) {
                task.state = LoadingState::Failed;
                task.errorMessage = e.what();
            }
        }

        if (task.state == LoadingState::Complete || task.state == LoadingState::Failed) {
            it = m_loadingTasks.erase(it);
        }
        else {
            ++it;
        }
    }
}
