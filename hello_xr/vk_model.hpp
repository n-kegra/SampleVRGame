#pragma once

#include <tiny_gltf.h>

inline auto getVkTexFilterFromGltfTexFilter(int filter) {
    switch (filter) {
    case TINYGLTF_TEXTURE_FILTER_NEAREST:
        return vk::Filter::eNearest;
    case TINYGLTF_TEXTURE_FILTER_LINEAR:
        return vk::Filter::eLinear;
    default:
        return vk::Filter::eLinear;
    }
}
inline auto getVkIndexTypeFromGltfComponent(int component) {
    switch (component)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return vk::IndexType::eUint8EXT;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return vk::IndexType::eUint16;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return vk::IndexType::eUint32;
    default:
        return vk::IndexType::eUint32;
    }
}

class ModelData {
    std::vector<Buffer> buffers;
    std::vector<vk::Buffer> vkBuffers;
    std::vector<TextureImage> textureImages;
    std::vector<vk::UniqueSampler> textureSamplers;
    vk::UniqueDescriptorPool descPool;
    std::vector<vk::UniqueDescriptorSet> materialDescSets;
    std::vector<glm::vec3> materialBaseColors;

    tinygltf::Scene defaultScene;
    std::vector<tinygltf::Node> nodes;
    std::vector<tinygltf::Mesh> meshes;

    struct PrimitiveRendering {
        vk::Buffer indexBuf;
        vk::DeviceSize indexBufOffset;
        size_t count;
        vk::IndexType indexType;
        XrMatrix4x4f matrix;
        std::array<vk::Buffer, 3> vertBufs;
        std::array<vk::DeviceSize, 3> vertBufOffsets;
    };
    std::vector<std::vector<PrimitiveRendering>> primitiveRenderings;

    static std::optional<TextureImage> defaultTexture;
    static vk::UniqueSampler defaultSampler;

    void loadMesh(const tinygltf::Model& model, const tinygltf::Mesh& mesh) {
        for (const auto& primitive : mesh.primitives) {
            PrimitiveRendering renderDat;

            {
                const auto& accessor = model.accessors[primitive.indices];
                const auto& bufView = model.bufferViews[accessor.bufferView];

                renderDat.indexBuf = buffers[bufView.buffer].get();
                renderDat.indexBufOffset = bufView.byteOffset;
                renderDat.count = accessor.count;
                renderDat.indexType = getVkIndexTypeFromGltfComponent(accessor.componentType);
            }

            for (const auto& attr : primitive.attributes) {
                std::cout << "attribute: " << attr.first << std::endl;
                const auto& accessor = model.accessors[attr.second];
                const auto& bufView = model.bufferViews[accessor.bufferView];

                int binding = -1;

                if (attr.first == "POSITION")
                    binding = 0;
                else if (attr.first == "NORMAL")
                    binding = 1;
                else if (attr.first == "TEXCOORD_0")
                    binding = 2;

                if (binding != -1) {
                    renderDat.vertBufs[binding] = buffers[bufView.buffer].get();
                    renderDat.vertBufOffsets[binding] = bufView.byteOffset;
                }
            }

            primitiveRenderings[primitive.material].push_back(renderDat);
        }
    }
    void loadNode(const tinygltf::Model& model, const tinygltf::Node& node) {
        if (0 <= node.mesh && node.mesh < model.meshes.size())
            loadMesh(model, model.meshes[node.mesh]);

        for (const auto& child : node.children) {
            if (0 <= child && child < model.nodes.size())
                loadNode(model, model.nodes[child]);
        }
    }

    void loadBuffers(const tinygltf::Model& model, vk::Device device, const Allocator& allocator, CommandBuffer& cmdBuf, vk::Queue queue) {
        for (const auto& buffer : model.buffers) {
            buffers.emplace_back(device, allocator, buffer.data.size(), vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer);
            buffers.back().pasteViaStaging<true>(cmdBuf, queue, reinterpret_cast<const std::byte*>(buffer.data.data()), buffer.data.size());
            vkBuffers.push_back(buffers.back().get());
        }
    }
    void loadImages(const tinygltf::Model& model, vk::Device device, const Allocator& allocator, CommandBuffer& cmdBuf, vk::Queue queue) {
        for (const auto& image : model.images) {
            assert(image.component == 4);
            assert(image.bits == 8);

            vk::Extent3D extent;
            extent.width = image.width;
            extent.height = image.height;
            extent.depth = 1;

            std::vector<std::byte> tmp(image.image.size());
            for(size_t i = 0; i < image.image.size(); i++)
                tmp[i] = static_cast<std::byte>(image.image[i]);
            textureImages.emplace_back(device, allocator, cmdBuf, queue, extent, tmp);
        }

        if (!defaultTexture) {
            vk::Extent3D extent;
            extent.width = 1;
            extent.height = 1;
            extent.depth = 1;

            std::vector<std::byte> dat(4, std::byte(0));

            defaultTexture.emplace(device, allocator, cmdBuf, queue, extent, dat);
        }
    }
    void loadSamplers(const tinygltf::Model& model, vk::Device device) {
        for (const auto& sampler : model.samplers) {
            vk::SamplerCreateInfo createInfo;

            createInfo.magFilter = getVkTexFilterFromGltfTexFilter(sampler.magFilter);
            createInfo.minFilter = getVkTexFilterFromGltfTexFilter(sampler.minFilter);
            createInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            createInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            createInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            createInfo.anisotropyEnable = VK_FALSE;
            createInfo.maxAnisotropy = 1.0f;
            createInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
            createInfo.unnormalizedCoordinates = VK_FALSE;
            createInfo.compareEnable = VK_FALSE;
            createInfo.compareOp = vk::CompareOp::eAlways;
            createInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
            createInfo.mipLodBias = 0.0f;
            createInfo.minLod = 0.0f;
            createInfo.maxLod = 0.0f;

            textureSamplers.push_back(device.createSamplerUnique(createInfo));
        }

        if (!defaultSampler) {
            vk::SamplerCreateInfo createInfo;

            createInfo.magFilter = vk::Filter::eLinear;
            createInfo.minFilter = vk::Filter::eLinear;
            createInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            createInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            createInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            createInfo.anisotropyEnable = VK_FALSE;
            createInfo.maxAnisotropy = 1.0f;
            createInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
            createInfo.unnormalizedCoordinates = VK_FALSE;
            createInfo.compareEnable = VK_FALSE;
            createInfo.compareOp = vk::CompareOp::eAlways;
            createInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
            createInfo.mipLodBias = 0.0f;
            createInfo.minLod = 0.0f;
            createInfo.maxLod = 0.0f;

            defaultSampler = device.createSamplerUnique(createInfo);
        }
    }
    void loadMaterialDescs(const tinygltf::Model& model, vk::Device device, vk::DescriptorSetLayout layout) {
        vk::DescriptorPoolSize poolSizes[1];
        poolSizes[0].type = vk::DescriptorType::eCombinedImageSampler;
        poolSizes[0].descriptorCount = model.materials.size();

        vk::DescriptorPoolCreateInfo poolCreateInfo;
        poolCreateInfo.poolSizeCount = std::size(poolSizes);
        poolCreateInfo.pPoolSizes = poolSizes;
        poolCreateInfo.maxSets = model.materials.size();
        poolCreateInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;

        descPool = device.createDescriptorPoolUnique(poolCreateInfo);

        std::vector<vk::DescriptorSetLayout> layouts(model.materials.size(), layout);

        vk::DescriptorSetAllocateInfo allocInfo;
        allocInfo.descriptorPool = descPool.get();
        allocInfo.descriptorSetCount = layouts.size();
        allocInfo.pSetLayouts = layouts.data();
        materialDescSets = device.allocateDescriptorSetsUnique(allocInfo);

        materialBaseColors.resize(model.materials.size());

        for (uint32_t i = 0; const auto & material : model.materials) {
            vk::DescriptorImageInfo imageInfo;
            imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            if (material.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                const auto& texture = model.textures[material.pbrMetallicRoughness.baseColorTexture.index];

                imageInfo.imageView = textureImages[texture.source].getImageView();
                imageInfo.sampler = textureSamplers[texture.sampler].get();
            }
            else {
                imageInfo.imageView = defaultTexture->getImageView();
                imageInfo.sampler = defaultSampler.get();
            }

            vk::WriteDescriptorSet samplerWrite;
            samplerWrite.dstSet = materialDescSets[i].get();
            samplerWrite.dstBinding = 0;
            samplerWrite.dstArrayElement = 0;
            samplerWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            samplerWrite.descriptorCount = 1;
            samplerWrite.pImageInfo = &imageInfo;

            device.updateDescriptorSets({ samplerWrite }, {});

            materialBaseColors[i].r = material.pbrMetallicRoughness.baseColorFactor[0];
            materialBaseColors[i].g = material.pbrMetallicRoughness.baseColorFactor[1];
            materialBaseColors[i].b = material.pbrMetallicRoughness.baseColorFactor[2];

            i++;
        }
    }

    void loadModel(vk::Device device, const Allocator& allocator, CommandBuffer& cmdBuf, vk::Queue queue, vk::DescriptorSetLayout layout, std::filesystem::path path) {
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;
        std::string err;
        std::string warn;

        auto fileDat = file_get_contents(path);
        bool ret = loader.LoadBinaryFromMemory(&model, &err, &warn, reinterpret_cast<const unsigned char*>(fileDat.data()), fileDat.size());
        if (!err.empty())
            std::cerr << err << std::endl;
        if (!warn.empty())
            std::cerr << warn << std::endl;
        if (!ret)
            throw std::runtime_error("Failed to load model");

        loadBuffers(model, device, allocator, cmdBuf, queue);
        loadImages(model, device, allocator, cmdBuf, queue);
        loadSamplers(model, device);
        loadMaterialDescs(model, device, layout);

        primitiveRenderings.resize(model.materials.size());

        const tinygltf::Scene& scene = model.scenes[model.defaultScene];
        for (const auto& node : scene.nodes) {
            loadNode(model, model.nodes[node]);
        }
    }

public:
    ModelData(vk::Device device, const Allocator& allocator, CommandBuffer& cmdBuf, vk::Queue queue, vk::DescriptorSetLayout layout, std::filesystem::path path) {
        loadModel(device, allocator, cmdBuf, queue, layout, path);
    }

    void DrawModel(const vk::CommandBuffer& cmdBuf, vk::PipelineLayout layout, const glm::mat4 &mvp) {
        PushConstantData pcd;

        pcd.mvp = mvp;
//        pcd.invMvp = glm::inverse(mvp);
        //pcd.lightDir = light;

        for (int i = 0; const auto & materialDesc : materialDescSets) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, { materialDesc.get() }, {});

            pcd.baseColor = materialBaseColors[i];

            for (const auto& prim : primitiveRenderings[i]) {
                cmdBuf.bindVertexBuffers(0, prim.vertBufs, prim.vertBufOffsets);
                cmdBuf.bindIndexBuffer(prim.indexBuf, prim.indexBufOffset, prim.indexType);
                cmdBuf.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(pcd), &pcd);
                cmdBuf.drawIndexed(prim.count, 1, 0, 0, 0);
            }
            i++;
        }
    }
};
