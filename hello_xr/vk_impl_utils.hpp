#pragma once

#include <stb_image.h>
#include <fmt/format.h>

inline auto getVkVersionString(uint32_t version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
}

struct PushConstantData {
    glm::mat4 mvp;
    glm::vec3 baseColor;
//    glm::mat4 invMvp;
//    glm::vec3 lightDir;
};

class Allocator {
    const vk::Device device;
    const vk::PhysicalDeviceMemoryProperties props;

    uint32_t findSuitableMemory(vk::MemoryRequirements memReq, vk::MemoryPropertyFlags flag) const {
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            if (((memReq.memoryTypeBits >> i) & 1) &&
                (props.memoryTypes[i].propertyFlags & flag) == flag) {
                return i;
            }
        }
        throw std::runtime_error("Could not find suitable memory type");
    }
public:
    Allocator(const vk::Device& _device, vk::PhysicalDevice physDevice) : device(_device), props(physDevice.getMemoryProperties()) {}
    auto allocate(vk::MemoryRequirements memReq, vk::MemoryPropertyFlags flag = {}) const {
        vk::MemoryAllocateInfo allocInfo;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findSuitableMemory(memReq, flag);
        return device.allocateMemoryUnique(allocInfo);
    }
};

class CommandBuffer
{
    vk::UniqueCommandPool cmdPool;
    std::vector<vk::UniqueCommandBuffer> cmdBufs;
    std::vector<vk::UniqueFence> fences;
    uint32_t count;
public:
    CommandBuffer(vk::Device device, uint32_t num = 1) : fences(num) {
        vk::CommandPoolCreateInfo poolCreateInfo;
        poolCreateInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        cmdPool = device.createCommandPoolUnique(poolCreateInfo);

        vk::CommandBufferAllocateInfo allocInfo;
        allocInfo.commandBufferCount = 1;
        allocInfo.commandPool = cmdPool.get();
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        cmdBufs = device.allocateCommandBuffersUnique(allocInfo);

        for (uint32_t i = 0; i < num; i++) {
            vk::FenceCreateInfo fenceCreateInfo;
            fenceCreateInfo.flags = vk::FenceCreateFlagBits::eSignaled;
            fences[i] = device.createFenceUnique(fenceCreateInfo);
        }
    }
    ~CommandBuffer() {

    }
    template<class F>
    auto exec(vk::Device device, const vk::Queue& queue, F func, bool async = true) {
        device.waitForFences({ fences[count].get() }, VK_TRUE, UINT64_MAX);
        device.resetFences({ fences[count].get() });
        cmdBufs[count]->reset();

        vk::CommandBufferBeginInfo beginInfo;
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        cmdBufs[count]->begin(beginInfo);

        func(cmdBufs[count].get());

        cmdBufs[count]->end();

        vk::SubmitInfo submitInfo;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBufs[count].get();
        queue.submit({ submitInfo }, fences[count].get());

        if (!async)
            device.waitForFences({ fences[count].get() }, VK_TRUE, UINT64_MAX);

        count++;
        count %= cmdBufs.size();
    }
    template<class F>
    auto execSync(vk::Device device, const vk::Queue& queue, F func) {
        exec(device, queue, std::forward<F>(func), false);
    }
};

class Buffer {
    vk::UniqueBuffer buf;
    vk::UniqueDeviceMemory mem;
    vk::DeviceSize size;
    const vk::Device device;
    const Allocator& allocator;
    std::unique_ptr<Buffer> staging;
public:
    Buffer(vk::Device _device, const Allocator& _allocator, vk::DeviceSize _size, vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags memProps = vk::MemoryPropertyFlagBits::eDeviceLocal, vk::SharingMode share = vk::SharingMode::eExclusive)
        : device(_device), allocator(_allocator), size(_size) {
        vk::BufferCreateInfo createInfo;
        createInfo.size = size;
        createInfo.usage = usage | vk::BufferUsageFlagBits::eTransferDst;
        createInfo.sharingMode = share;
        buf = device.createBufferUnique(createInfo);

        auto memReq = device.getBufferMemoryRequirements(buf.get());

        mem = allocator.allocate(memReq, memProps);
        device.bindBufferMemory(buf.get(), mem.get(), 0);
    }
    auto& get() const {
        return buf.get();
    }
    void paste(const std::byte* src, vk::DeviceSize dataSize, vk::DeviceSize offset = 0) const {
        auto dest = device.mapMemory(mem.get(), offset, dataSize);

        std::memcpy(dest, src, dataSize);

        vk::MappedMemoryRange range;
        range.memory = mem.get();
        range.offset = 0;
        range.size = dataSize;
        device.flushMappedMemoryRanges({ range });

        device.unmapMemory(mem.get());
    }
    template<bool releaseImmediately = false>
    void pasteViaStaging(CommandBuffer& _cmdBuf, vk::Queue queue, const std::byte* src, vk::DeviceSize dataSize, vk::DeviceSize offset = 0) {
        if (!staging)
            staging = std::make_unique<Buffer>(device, allocator, size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible);
        staging->paste(src, dataSize, offset);
        _cmdBuf.execSync(device, queue, [&](const vk::CommandBuffer& cmdBuf) {
            vk::BufferCopy region;
            region.srcOffset = offset;
            region.dstOffset = offset;
            region.size = dataSize;
            cmdBuf.copyBuffer(staging->get(), buf.get(), { region });
            });
        if constexpr (releaseImmediately)
            releaseStagingBuffer();
    }
    void releaseStagingBuffer() {
        staging.reset();
    }
};

class ShaderModule {
    vk::UniqueShaderModule shader;
public:
    ShaderModule(vk::Device device, std::filesystem::path path) {
        auto spvFileData = file_get_contents(path);
        std::cout << "shader " << spvFileData.size() << std::endl;

        vk::ShaderModuleCreateInfo createInfo;
        createInfo.codeSize = spvFileData.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(spvFileData.data());
        shader = device.createShaderModuleUnique(createInfo);
    }
    const auto& get() const {
        return shader.get();
    }
    auto getStageCreateInfo(vk::ShaderStageFlagBits flag, const char* entry = "main") const {
        vk::PipelineShaderStageCreateInfo createInfo;
        createInfo.stage = flag;
        createInfo.module = shader.get();
        createInfo.pName = entry;
        return createInfo;
    }
};

class Image {
    vk::UniqueImage image;
    vk::UniqueDeviceMemory mem;
    vk::Extent3D extent;
    vk::Format format;
public:
    Image(vk::Device device, const Allocator& allocator, vk::Extent3D _extent, vk::Format _format, vk::ImageUsageFlags usage = {},
        vk::MemoryPropertyFlags memProps = vk::MemoryPropertyFlagBits::eDeviceLocal, vk::SharingMode share = vk::SharingMode::eExclusive)
        : extent(_extent), format(_format)
    {
        vk::ImageCreateInfo createInfo;
        createInfo.imageType = vk::ImageType::e2D;
        createInfo.extent = extent;
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.format = format;
        createInfo.tiling = vk::ImageTiling::eOptimal;
        createInfo.initialLayout = vk::ImageLayout::eUndefined;
        createInfo.usage = usage | vk::ImageUsageFlagBits::eTransferDst;
        createInfo.sharingMode = share;
        createInfo.samples = vk::SampleCountFlagBits::e1;
        image = device.createImageUnique(createInfo);

        auto memReq = device.getImageMemoryRequirements(image.get());

        mem = allocator.allocate(memReq, memProps);
        device.bindImageMemory(image.get(), mem.get(), 0);
    }
    auto CreateImageView(const vk::Device device, vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor, vk::ImageViewType viewType = vk::ImageViewType::e2D) const {
        vk::ImageViewCreateInfo viewCreateInfo;
        viewCreateInfo.image = image.get();
        viewCreateInfo.viewType = viewType;
        viewCreateInfo.format = format;
        viewCreateInfo.subresourceRange.aspectMask = aspect;
        viewCreateInfo.subresourceRange.baseMipLevel = 0;
        viewCreateInfo.subresourceRange.levelCount = 1;
        viewCreateInfo.subresourceRange.baseArrayLayer = 0;
        viewCreateInfo.subresourceRange.layerCount = 1;

        return device.createImageViewUnique(viewCreateInfo);
    }
    const auto& get() const {
        return image.get();
    }
    const auto& getExtent() const {
        return extent;
    }
};

class TextureImage {
    std::optional<Image> image;
    vk::UniqueImageView imageView;

    void CopyFromBuffer(const vk::Device device, CommandBuffer& cmdBuf, const vk::Queue queue, const Buffer& buffer, const vk::Extent3D extent) {
        cmdBuf.execSync(device, queue, [&](const vk::CommandBuffer& cmdBuf) {
            {
                vk::ImageMemoryBarrier barrier;
                barrier.oldLayout = vk::ImageLayout::eUndefined;
                barrier.srcAccessMask = {};
                barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
                barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

                barrier.image = image->get();
                barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;

                cmdBuf.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTopOfPipe,
                    vk::PipelineStageFlagBits::eTransfer, {},
                    {},
                    {},
                    { barrier });
            }

            vk::BufferImageCopy region;
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = vk::Offset3D{ 0, 0, 0 };
            region.imageExtent = extent;
            cmdBuf.copyBufferToImage(buffer.get(), image->get(), vk::ImageLayout::eTransferDstOptimal, { region });

            {
                vk::ImageMemoryBarrier barrier;
                barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
                barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
                barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

                barrier.image = image->get();
                barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;

                cmdBuf.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eFragmentShader, {},
                    {},
                    {},
                    { barrier });
            }
            });
    }
    void CreateImage(const vk::Device device, const Allocator& allocator, const vk::Extent3D extent) {
        image.emplace(device, allocator, extent, vk::Format::eR8G8B8A8Srgb, vk::ImageUsageFlagBits::eSampled);
    }
    void CreateImageView(const vk::Device device) {
        imageView = image->CreateImageView(device);
    }
public:
    TextureImage(vk::Device device, const Allocator& allocator, CommandBuffer& cmdBuf, vk::Queue queue, std::filesystem::path path, vk::SharingMode share = vk::SharingMode::eExclusive)
    {
        int width, height, channels;
        const auto imgFileData = file_get_contents(path);
        const auto imgData = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(imgFileData.data()), imgFileData.size(), &width, &height, &channels, STBI_rgb_alpha);

        vk::DeviceSize size = vk::DeviceSize(width) * vk::DeviceSize(height) * 4;
        Buffer staging{ device, allocator, size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible };
        staging.paste(reinterpret_cast<const std::byte*>(imgData), size);

        stbi_image_free(imgData);

        vk::Extent3D extent;
        extent.width = width;
        extent.height = height;
        extent.depth = 1;

        CreateImage(device, allocator, extent);
        CopyFromBuffer(device, cmdBuf, queue, staging, extent);
        CreateImageView(device);
    }
    TextureImage(vk::Device device, const Allocator& allocator, CommandBuffer& cmdBuf, vk::Queue queue, vk::Extent3D extent, const std::vector<std::byte>& imgData, vk::SharingMode share = vk::SharingMode::eExclusive)
    {
        vk::DeviceSize size = imgData.size();
        Buffer staging{ device, allocator, size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible };
        staging.paste(imgData.data(), size);

        CreateImage(device, allocator, extent);
        CopyFromBuffer(device, cmdBuf, queue, staging, extent);
        CreateImageView(device);
    }
    auto& get() const {
        return image->get();
    }
    auto& getImageView() const {
        return imageView.get();
    }
    const auto& getExtent() const {
        return image->getExtent();
    }
};

class RenderProc {
    const vk::Device device;
    const vk::Format format;

    ShaderModule vertShader, fragShader;

    vk::UniqueRenderPass renderpass;
    vk::UniqueDescriptorSetLayout descSetLayout;
    vk::UniquePipelineLayout pipelineLayout;

    void CreateRenderpass() {
        vk::AttachmentDescription attachments[2];
        attachments[0].format = format;
        attachments[0].samples = vk::SampleCountFlagBits::e1;
        attachments[0].loadOp = vk::AttachmentLoadOp::eClear;
        attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
        attachments[0].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        attachments[0].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        attachments[0].initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
        attachments[0].finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
        attachments[1].format = vk::Format::eD32Sfloat;
        attachments[1].samples = vk::SampleCountFlagBits::e1;
        attachments[1].loadOp = vk::AttachmentLoadOp::eClear;
        attachments[1].storeOp = vk::AttachmentStoreOp::eDontCare;
        attachments[1].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        attachments[1].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        attachments[1].initialLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        attachments[1].finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

        vk::AttachmentReference subpass0_colorAttachmentRefs[1];
        subpass0_colorAttachmentRefs[0].attachment = 0;
        subpass0_colorAttachmentRefs[0].layout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::AttachmentReference subpass0_depthAttachmentRef;
        subpass0_depthAttachmentRef.attachment = 1;
        subpass0_depthAttachmentRef.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

        vk::SubpassDescription subpasses[1];
        subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
        subpasses[0].colorAttachmentCount = std::size(subpass0_colorAttachmentRefs);
        subpasses[0].pColorAttachments = subpass0_colorAttachmentRefs;
        subpasses[0].pDepthStencilAttachment = &subpass0_depthAttachmentRef;

        vk::SubpassDependency dependency[1];
        dependency[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency[0].dstSubpass = 0;
        dependency[0].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests;
        dependency[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests;
        dependency[0].srcAccessMask = {};
        dependency[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;

        vk::RenderPassCreateInfo createInfo;
        createInfo.attachmentCount = std::size(attachments);
        createInfo.pAttachments = attachments;
        createInfo.subpassCount = std::size(subpasses);
        createInfo.pSubpasses = subpasses;
        createInfo.dependencyCount = std::size(dependency);
        createInfo.pDependencies = dependency;

        this->renderpass = device.createRenderPassUnique(createInfo);
    }

    void CreateDescriptorSetLayout() {
        vk::DescriptorSetLayoutBinding samplerBinding;
        samplerBinding.binding = 0;
        samplerBinding.descriptorCount = 1;
        samplerBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        samplerBinding.pImmutableSamplers = nullptr;
        samplerBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;

        vk::DescriptorSetLayoutBinding bindings[] = {
            samplerBinding,
        };

        vk::DescriptorSetLayoutCreateInfo createInfo;
        createInfo.bindingCount = std::size(bindings);
        createInfo.pBindings = bindings;

        descSetLayout = device.createDescriptorSetLayoutUnique(createInfo);
    }

    void CreatePipelineLayout() {
        vk::PushConstantRange pcr;
        pcr.offset = 0;
        pcr.size = sizeof(PushConstantData);
        pcr.stageFlags = vk::ShaderStageFlagBits::eVertex;

        auto pcrs = { pcr };

        vk::PipelineLayoutCreateInfo layoutCreateInfo;
        layoutCreateInfo.setLayoutCount = 1;
        layoutCreateInfo.pSetLayouts = &descSetLayout.get();
        layoutCreateInfo.pushConstantRangeCount = pcrs.size();
        layoutCreateInfo.pPushConstantRanges = pcrs.begin();

        pipelineLayout = device.createPipelineLayoutUnique(layoutCreateInfo);
    }

public:
    RenderProc(const vk::Device _device, const vk::Format _format)
        : device(_device), format(_format),
        vertShader(device, "shader.vert.spv"), fragShader(device, "shader.frag.spv")
    {
        CreateRenderpass();
        CreateDescriptorSetLayout();
        CreatePipelineLayout();
    }

    auto CreatePipeline(vk::Extent2D extent) const {
        vk::Viewport viewports[1];
        viewports[0].x = 0.0;
        viewports[0].y = 0.0;
        viewports[0].minDepth = 0.0;
        viewports[0].maxDepth = 1.0;
        viewports[0].width = extent.width;
        viewports[0].height = extent.height;

        vk::Rect2D scissors[1];
        scissors[0].offset = vk::Offset2D{ 0, 0 };
        scissors[0].extent = extent;

        vk::PipelineViewportStateCreateInfo viewportState;
        viewportState.viewportCount = 1;
        viewportState.pViewports = viewports;
        viewportState.scissorCount = 1;
        viewportState.pScissors = scissors;

        vk::VertexInputAttributeDescription attrDesc[3];
        // position
        attrDesc[0].binding = 0;
        attrDesc[0].location = 0;
        attrDesc[0].format = vk::Format::eR32G32B32Sfloat;
        attrDesc[0].offset = 0;
        // normal
        attrDesc[1].binding = 1;
        attrDesc[1].location = 1;
        attrDesc[1].format = vk::Format::eR32G32B32Sfloat;
        attrDesc[1].offset = 0;
        // texcoord
        attrDesc[2].binding = 2;
        attrDesc[2].location = 2;
        attrDesc[2].format = vk::Format::eR32G32Sfloat;
        attrDesc[2].offset = 0;

        vk::VertexInputBindingDescription bindDesc[3];
        // position
        bindDesc[0].binding = 0;
        bindDesc[0].stride = sizeof(glm::vec3);
        bindDesc[0].inputRate = vk::VertexInputRate::eVertex;
        // normal
        bindDesc[1].binding = 1;
        bindDesc[1].stride = sizeof(glm::vec3);
        bindDesc[1].inputRate = vk::VertexInputRate::eVertex;
        // texcoord
        bindDesc[2].binding = 2;
        bindDesc[2].stride = sizeof(glm::vec2);
        bindDesc[2].inputRate = vk::VertexInputRate::eVertex;

        vk::PipelineVertexInputStateCreateInfo vertexInputInfo;
        vertexInputInfo.vertexAttributeDescriptionCount = std::size(attrDesc);
        vertexInputInfo.pVertexAttributeDescriptions = attrDesc;
        vertexInputInfo.vertexBindingDescriptionCount = std::size(bindDesc);
        vertexInputInfo.pVertexBindingDescriptions = bindDesc;

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly;
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        inputAssembly.primitiveRestartEnable = false;

        vk::PipelineRasterizationStateCreateInfo rasterizer;
        rasterizer.depthClampEnable = false;
        rasterizer.rasterizerDiscardEnable = false;
        rasterizer.polygonMode = vk::PolygonMode::eFill;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = vk::CullModeFlagBits::eBack;
        rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
        rasterizer.depthBiasEnable = false;

        vk::PipelineMultisampleStateCreateInfo multisample;
        multisample.sampleShadingEnable = false;
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

        vk::PipelineColorBlendAttachmentState blendattachment[1];
        blendattachment[0].colorWriteMask =
            vk::ColorComponentFlagBits::eA |
            vk::ColorComponentFlagBits::eR |
            vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB;
        blendattachment[0].blendEnable = false;

        vk::PipelineDepthStencilStateCreateInfo depthStencil;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.minDepthBounds = 0.0f;
        depthStencil.maxDepthBounds = 1.0f;
        depthStencil.stencilTestEnable = VK_FALSE;
        depthStencil.front = vk::StencilOpState{};
        depthStencil.back = vk::StencilOpState{};

        vk::PipelineColorBlendStateCreateInfo blend;
        blend.logicOpEnable = false;
        blend.attachmentCount = 1;
        blend.pAttachments = blendattachment;

        vk::PipelineShaderStageCreateInfo shaderStage[] = {
            vertShader.getStageCreateInfo(vk::ShaderStageFlagBits::eVertex),
            fragShader.getStageCreateInfo(vk::ShaderStageFlagBits::eFragment),
        };

        vk::GraphicsPipelineCreateInfo pipelineCreateInfo;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
        pipelineCreateInfo.pInputAssemblyState = &inputAssembly;
        pipelineCreateInfo.pRasterizationState = &rasterizer;
        pipelineCreateInfo.pMultisampleState = &multisample;
        pipelineCreateInfo.pColorBlendState = &blend;
        pipelineCreateInfo.pDepthStencilState = &depthStencil;
        pipelineCreateInfo.layout = pipelineLayout.get();
        pipelineCreateInfo.stageCount = std::size(shaderStage);
        pipelineCreateInfo.pStages = shaderStage;
        pipelineCreateInfo.renderPass = renderpass.get();
        pipelineCreateInfo.subpass = 0;

        return device.createGraphicsPipelineUnique(nullptr, pipelineCreateInfo).value;
    }

    auto getRenderpass() const {
        return renderpass.get();
    }

    auto getDescriptorSetLayout() const {
        return descSetLayout.get();
    }

    auto getPipelineLayout() const {
        return pipelineLayout.get();
    }

    auto getFormat() const {
        return format;
    }
};

class SwapchainRenderTargets {
    vk::Format format;
    vk::Extent2D extent;
    vk::Device device;

    vk::RenderPass renderpass;
    vk::UniquePipeline pipeline;
    std::vector<vk::Image> swapchainImages;

    struct RenderTarget {
        vk::UniqueImageView imgView;
        vk::UniqueFramebuffer frameBuf;
    };
    std::vector<RenderTarget> renderTargets;

    std::optional<Image> depthImage;
    vk::UniqueImageView depthImageView;

    void CreateDepthBuffer(const vk::Device device, const vk::Extent2D extent, const Allocator& allocator) {
        depthImage.emplace(device, allocator, vk::Extent3D{ extent.width, extent.height, 1 }, vk::Format::eD32Sfloat, vk::ImageUsageFlagBits::eDepthStencilAttachment);
        depthImageView = depthImage->CreateImageView(device, vk::ImageAspectFlagBits::eDepth);
    };

    void CreateFrameBuffer(uint32_t index) {
        vk::ImageViewCreateInfo imgViewCreateInfo;
        imgViewCreateInfo.image = swapchainImages[index];
        imgViewCreateInfo.viewType = vk::ImageViewType::e2D;
        imgViewCreateInfo.format = format;
        imgViewCreateInfo.components.a = vk::ComponentSwizzle::eA;
        imgViewCreateInfo.components.r = vk::ComponentSwizzle::eR;
        imgViewCreateInfo.components.g = vk::ComponentSwizzle::eG;
        imgViewCreateInfo.components.b = vk::ComponentSwizzle::eB;
        imgViewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        imgViewCreateInfo.subresourceRange.baseMipLevel = 0;
        imgViewCreateInfo.subresourceRange.levelCount = 1;
        imgViewCreateInfo.subresourceRange.baseArrayLayer = 0;
        imgViewCreateInfo.subresourceRange.layerCount = 1;
        auto imgView = device.createImageViewUnique(imgViewCreateInfo);

        vk::ImageView attachments[] = {
            imgView.get(),
            this->depthImageView.get()
        };

        vk::FramebufferCreateInfo createInfo;
        createInfo.width = extent.width;
        createInfo.height = extent.height;
        createInfo.layers = 1;
        createInfo.renderPass = this->renderpass;
        createInfo.attachmentCount = std::size(attachments);
        createInfo.pAttachments = attachments;
        auto frameBuf = device.createFramebufferUnique(createInfo);

        this->renderTargets[index].imgView = std::move(imgView);
        this->renderTargets[index].frameBuf = std::move(frameBuf);
    }

public:
    SwapchainRenderTargets(const vk::Device _device, const std::vector<vk::Image>& _swapchainImages, const vk::Extent2D _extent, const Allocator& allocator, const RenderProc& renderproc) :
        device(_device), format(renderproc.getFormat()), extent(_extent),
        swapchainImages(_swapchainImages), renderTargets(_swapchainImages.size())
    {
        renderpass = renderproc.getRenderpass();
        pipeline = renderproc.CreatePipeline(extent);
        CreateDepthBuffer(device, extent, allocator);
        for (uint32_t i = 0; i < swapchainImages.size(); i++)
            CreateFrameBuffer(i);
    }

    void beginRenderPass(const vk::CommandBuffer cmdBuf, uint32_t imageIndex) {
        vk::ClearValue clearVal[2];
        clearVal[0].color.float32[0] = 0.05f;
        clearVal[0].color.float32[1] = 0.05f;
        clearVal[0].color.float32[2] = 0.1f;
        clearVal[0].color.float32[3] = 1.0f;
        clearVal[1].depthStencil.depth = 1.0f;
        clearVal[1].depthStencil.stencil = 0.0f;

        vk::RenderPassBeginInfo renderpassBeginInfo;
        renderpassBeginInfo.renderPass = this->renderpass;
        renderpassBeginInfo.framebuffer = this->renderTargets[imageIndex].frameBuf.get();
        renderpassBeginInfo.clearValueCount = std::size(clearVal);
        renderpassBeginInfo.pClearValues = clearVal;
        renderpassBeginInfo.renderArea.offset = vk::Offset2D{ 0, 0 };
        renderpassBeginInfo.renderArea.extent = extent;

        cmdBuf.beginRenderPass(renderpassBeginInfo, vk::SubpassContents::eInline);
        cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get());
    }

    void endRenderPass(const vk::CommandBuffer cmdBuf) const {
        cmdBuf.endRenderPass();
    }

    const auto& getExtent() const {
        return extent;
    }
};
