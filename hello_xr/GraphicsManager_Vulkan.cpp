#include <iostream>
#include <fmt/format.h>

#define XR_NULL_ASYNC_REQUEST_ID_FB 0

#ifdef _WIN32
#include <Windows.h>
#include <wrl/client.h>
#endif
#ifdef XR_USE_PLATFORM_ANDROID
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/native_window.h>
#include <jni.h>
#include <sys/system_properties.h>
#endif

#include <vulkan/vulkan.hpp>
#include <openxr/openxr_platform.h>
#include <openxr/openxr.hpp>
#include <glm/glm.hpp>
#include "xr_linear.h"

#include "Game.hpp"
#include "GraphicsManager_Vulkan.hpp"
#include "utils.hpp"

#include "vk_impl_utils.hpp"
#include "vk_model.hpp"

std::optional<TextureImage> ModelData::defaultTexture;
vk::UniqueSampler ModelData::defaultSampler;

auto CreateTranslationRotationScale(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
    return glm::translate(glm::identity<glm::mat4>(), translation)
        * glm::mat4(rotation)
        * glm::scale(glm::identity<glm::mat4>(), scale);
}
auto CreateTranslationRotationScale(const xr::Posef& pose, const glm::vec3& scale) {
    return CreateTranslationRotationScale(toG(pose.position), toG(pose.orientation), scale);
}

class VulkanManager : public IGraphicsManager {
    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;
    vk::Device device;
    vk::Queue queue;

    std::optional<Allocator> allocator;
    std::optional<CommandBuffer> cmdBufs;

    std::optional<RenderProc> renderproc;
    std::vector<std::vector<xr::SwapchainImageVulkanKHR>> swapchainImages;
    std::vector<SwapchainRenderTargets> renderTargets;

    std::optional<ModelData> handModel;
    std::optional<ModelData> beamModel;
    std::optional<ModelData> tgtModel;
    std::optional<ModelData> stageModel;

    std::vector<ModelData> modelDb;

    void CreateInstance(xr::Instance xrInstance, xr::SystemId systemId) {
        auto graphicsRequirements = xrInstance.getVulkanGraphicsRequirements2KHR(systemId, xr::DispatchLoaderDynamic{ xrInstance });

        std::cout << fmt::format("Required Vulkan Version: {} ~ {}", to_string(graphicsRequirements.minApiVersionSupported), to_string(graphicsRequirements.maxApiVersionSupported)) << std::endl;

        std::vector<const char*> exts = { "VK_EXT_debug_report" };
        std::vector<const char*> layers = { /* "VK_LAYER_KHRONOS_validation" */ };

        vk::ApplicationInfo appInfo;
        appInfo.pApplicationName = "XRTest";
        appInfo.apiVersion = 1;
        appInfo.pEngineName = "XRTest";
        appInfo.engineVersion = 1;
        appInfo.apiVersion = VK_API_VERSION_1_0;


        vk::InstanceCreateInfo createInfo{};
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = exts.size();
        createInfo.ppEnabledExtensionNames = exts.data();
        createInfo.enabledLayerCount = layers.size();
        createInfo.ppEnabledLayerNames = layers.data();

        const VkInstanceCreateInfo c_createInfo = (VkInstanceCreateInfo)createInfo;

        xr::VulkanInstanceCreateInfoKHR xrCreateInfo{};
        xrCreateInfo.systemId = systemId;
        xrCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrCreateInfo.vulkanCreateInfo = &c_createInfo;

        VkInstance tmpInst;
        VkResult vkResult;

        xrInstance.createVulkanInstanceKHR(xrCreateInfo, &tmpInst, &vkResult, xr::DispatchLoaderDynamic{ xrInstance });
        if (vkResult != VK_SUCCESS)
            throw std::runtime_error(fmt::format("Vulkan Instance Creation Error(Vk): {}", to_string(vk::Result(vkResult))));

        this->instance = tmpInst;
    }

    void ChoosePhysicalDevice(xr::Instance xrInstance, xr::SystemId systemId) {
        xr::VulkanGraphicsDeviceGetInfoKHR getInfo{};
        getInfo.systemId = systemId;
        getInfo.vulkanInstance = instance;

        this->physicalDevice = xrInstance.getVulkanGraphicsDevice2KHR(getInfo, xr::DispatchLoaderDynamic{ xrInstance });

        auto prop = physicalDevice.getProperties();
        std::cout << fmt::format("Selected Device: {} (ID:{}) Type: {}", prop.deviceName, prop.deviceID, to_string(prop.deviceType)) << std::endl;
        std::cout << fmt::format("ApiVersion: {}, DriverVersion: {}", getVkVersionString(prop.apiVersion), getVkVersionString(prop.driverVersion)) << std::endl;
        std::cout << fmt::format("maxSamplerAnisotropy: {}", prop.limits.maxSamplerAnisotropy) << std::endl;
    }

    void PrepareQueue() {
        auto queueFamilies = physicalDevice.getQueueFamilyProperties();
        // auto queueFamilies2 = physicalDevice.getQueueFamilyProperties2();
        for (uint32_t i = 0; i < queueFamilies.size(); i++) {
            std::cout << fmt::format("QueueFamily {}: queue x{}, Graphics:{}, Transfer:{}, Compute:{}, Protected:{}, SparceBind:{}",
                i, queueFamilies[i].queueCount,
                bool(queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics),
                bool(queueFamilies[i].queueFlags & vk::QueueFlagBits::eTransfer),
                bool(queueFamilies[i].queueFlags & vk::QueueFlagBits::eCompute),
                bool(queueFamilies[i].queueFlags & vk::QueueFlagBits::eProtected),
                bool(queueFamilies[i].queueFlags & vk::QueueFlagBits::eSparseBinding)) << std::endl;

            if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics) {
                this->queueFamilyIndex = i;
            }
        }
        this->queueIndex = 0;
    }

    void CreateDevice(xr::Instance xrInstance, xr::SystemId systemId) {
        std::vector<float> queuePriorities = { 0.0f };

        vk::DeviceQueueCreateInfo queueInfo1;
        queueInfo1.queueCount = queuePriorities.size();
        queueInfo1.pQueuePriorities = queuePriorities.data();

        std::vector<vk::DeviceQueueCreateInfo> queueInfo = {
            queueInfo1
        };

        std::vector<const char*> exts = {};
        std::vector<const char*> layers = { "VK_LAYER_KHRONOS_validation" };

        vk::PhysicalDeviceFeatures features{};

        vk::DeviceCreateInfo createInfo{};
        createInfo.queueCreateInfoCount = queueInfo.size();
        createInfo.pQueueCreateInfos = queueInfo.data();
        createInfo.enabledExtensionCount = exts.size();
        createInfo.ppEnabledExtensionNames = exts.data();
        createInfo.enabledLayerCount = layers.size();
        createInfo.ppEnabledLayerNames = layers.data();

        const VkDeviceCreateInfo c_createInfo = (VkDeviceCreateInfo)createInfo;

        xr::VulkanDeviceCreateInfoKHR xrCreateInfo{};
        xrCreateInfo.systemId = systemId;
        xrCreateInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
        xrCreateInfo.vulkanCreateInfo = &c_createInfo;
        xrCreateInfo.vulkanPhysicalDevice = this->physicalDevice;

        VkDevice tmpDevice;
        VkResult vkResult;
        xrInstance.createVulkanDeviceKHR(xrCreateInfo, &tmpDevice, &vkResult, xr::DispatchLoaderDynamic{ xrInstance });
        if (vkResult != VK_SUCCESS)
            throw std::runtime_error(fmt::format("Vulkan Device Creation Error(Vk): {}", to_string(vk::Result(vkResult))));

        this->device = tmpDevice;
    }

    void GetQueue() {
        this->queue = this->device.getQueue(this->queueFamilyIndex, this->queueIndex);
    }

    void CreateCommandBuffer() {
        cmdBufs.emplace(device, 4);
    }

    class VulkanGraphicsProvider : public IGraphicsProvider {
        VulkanManager* const manager;
    public:
        VulkanGraphicsProvider(VulkanManager* p) : manager(p) {}
        ModelHandle LoadModel(const char* path) override {
            manager->modelDb.emplace_back(manager->device, manager->allocator.value(), manager->cmdBufs.value(), manager->queue, manager->renderproc->getDescriptorSetLayout(), path);
            return manager->modelDb.size() - 1;
        }
        void DrawModel(ModelHandle model, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale, const glm::mat4& mat) override {
            assert(manager->activeCmdBuf.has_value());

            auto mvp = manager->currentVp * CreateTranslationRotationScale(pos, rot, scale) * mat;

            manager->modelDb[model].DrawModel(manager->activeCmdBuf.value(), manager->renderproc->getPipelineLayout(), mvp);
        }
    };

    std::optional<vk::CommandBuffer> activeCmdBuf;
    std::optional<VulkanGraphicsProvider> provider;
    glm::mat4 currentVp;

public:
    VulkanManager(xr::Instance instance, xr::SystemId systemId) {
        CreateInstance(instance, systemId);
        ChoosePhysicalDevice(instance, systemId);
        PrepareQueue();
        CreateDevice(instance, systemId);
        GetQueue();
        CreateCommandBuffer();
        allocator.emplace(device, physicalDevice);
    }
    ~VulkanManager() {
        queue.waitIdle();
    }

    std::unique_ptr<xr::impl::InputStructBase> getXrGraphicsBinding() const override {
        xr::GraphicsBindingVulkanKHR graphicsBinding{};
        graphicsBinding.instance = this->instance;
        graphicsBinding.physicalDevice = this->physicalDevice;
        graphicsBinding.device = this->device;
        graphicsBinding.queueFamilyIndex = this->queueFamilyIndex;
        graphicsBinding.queueIndex = this->queueIndex;

        return std::make_unique<xr::GraphicsBindingVulkanKHR>(graphicsBinding);
    }

    int64_t chooseImageFormat(const std::vector<int64_t>& formats) const override {
        std::vector<vk::Format> supported = {
            vk::Format::eB8G8R8A8Srgb,
            vk::Format::eR8G8B8A8Srgb,
            vk::Format::eB8G8R8A8Unorm,
            vk::Format::eR8G8B8A8Unorm,
        };
        auto it = std::find_first_of(formats.begin(), formats.end(), supported.begin(), supported.end(), [](int64_t fmt1, vk::Format fmt2) { return vk::Format(fmt1) == fmt2; });
        return *it;
    }

    void InitializeRenderTargets(const std::vector<Swapchain>& swapchains, int64_t format) override {
        renderproc.emplace(device, vk::Format(format));

        for (const auto& swapchain : swapchains) {
            auto images = swapchain.handle->enumerateSwapchainImagesToVector<xr::SwapchainImageVulkanKHR>();
            auto& extent = swapchain.extent;

            std::vector<vk::Image> vkImages;
            std::transform(images.begin(), images.end(), std::back_inserter(vkImages),
                [](xr::SwapchainImageVulkanKHR image) { return vk::Image(image.image); });

            renderTargets.emplace_back(this->device, vkImages,
                vk::Extent2D{ static_cast<uint32_t>(extent.width), static_cast<uint32_t>(extent.height) }, allocator.value(), renderproc.value());
        }
    }

    void PrepareResources() override {
        provider.emplace(this);
        Game::init(provider.value());
    }

    void render(int viewIndex, int imageIndex, const xr::CompositionLayerProjectionView& view) override {
        XrMatrix4x4f proj;
        XrMatrix4x4f_CreateProjectionFov(&proj, GRAPHICS_VULKAN, view.fov, 0.05f, 100.0f);

        auto matView = glm::inverse(CreateTranslationRotationScale(view.pose, glm::vec3{ 1,1,1 }));
        currentVp = toG(proj) * matView;

        cmdBufs->exec(device, queue, [&](const vk::CommandBuffer& cmdBuf) {
            auto& renderTarget = this->renderTargets[viewIndex];
            activeCmdBuf = cmdBuf;

            renderTarget.beginRenderPass(cmdBuf, imageIndex);

            Game::draw(provider.value());

            renderTarget.endRenderPass(cmdBuf);
        });
    }
};

std::unique_ptr<IGraphicsManager> CreateGraphicsManager_Vulkan(xr::Instance instance, xr::SystemId systemId) {
    return std::make_unique<VulkanManager>(instance, systemId);
}

std::vector<const char*> GetGraphicsExtension_Vulkan()
{
    return { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
}
