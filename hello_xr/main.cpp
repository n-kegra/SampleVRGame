// Copyright (c) 2017-2020 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

//#define XR_NULL_ASYNC_REQUEST_ID_FB 0

#include <openxr/openxr.hpp>
#include <iostream>
#include <fmt/format.h>
#include "pch.h"
#include <array>

#include "xr_linear.h"
#include "logger.h"
#include "Game.hpp"
#include "GraphicsManager_Vulkan.hpp"
#include "utils.hpp"

#ifdef XR_USE_PLATFORM_ANDROID
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include <aaudio/AAudio.h>
AAssetManager* asset_manager;

class androidbuf : public std::streambuf {
public:
    enum { bufsize = 128 }; // ... or some other suitable buffer size
    androidbuf() { this->setp(buffer, buffer + bufsize - 1); }

private:
    int overflow(int c)
    {
        if (c == traits_type::eof()) {
            *this->pptr() = traits_type::to_char_type(c);
            this->sbumpc();
        }
        return this->sync()? traits_type::eof(): traits_type::not_eof(c);
    }

    int sync()
    {
        int rc = 0;
        if (this->pbase() != this->pptr()) {
            char writebuf[bufsize+1];
            memcpy(writebuf, this->pbase(), this->pptr() - this->pbase());
            writebuf[this->pptr() - this->pbase()] = '\0';

            rc = __android_log_write(ANDROID_LOG_INFO, "std", writebuf) > 0;
            this->setp(buffer, buffer + bufsize - 1);
        }
        return rc;
    }

    char buffer[bufsize];
};
#endif

#define XR_CHK_ERR(f) if (auto result = f; XR_FAILED(result)){ throw std::runtime_error(fmt::format("Err: {}, {} {}", to_string(result), __LINE__, #f)); }

auto SpaceToPose(xr::Space space, xr::Space baseSpace, xr::Time time) {
    auto location = space.locateSpace(baseSpace, time);

    if ((location.locationFlags & xr::SpaceLocationFlagBits::PositionValid) &&
        (location.locationFlags & xr::SpaceLocationFlagBits::OrientationValid)) {
        Game::Pose pose;
        pose.pos = toG(location.pose.position);
        pose.ori = toG(location.pose.orientation);
        return std::make_optional(pose);
    }
    else {
        return std::optional<Game::Pose>(std::nullopt);
    }
}

class App
{
    auto requiredExtensions() {
        auto requiredExts = GetGraphicsExtension_Vulkan();
#ifdef XR_USE_PLATFORM_ANDROID
        requiredExts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
        // requiredExts.push_back(XR_PICO_VIEW_STATE_EXT_ENABLE_EXTENSION_NAME);
        // requiredExts.push_back(XR_PICO_FRAME_END_INFO_EXT_EXTENSION_NAME);
        // requiredExts.push_back(XR_PICO_ANDROID_CONTROLLER_FUNCTION_EXT_ENABLE_EXTENSION_NAME);
        // requiredExts.push_back(XR_PICO_CONFIGS_EXT_EXTENSION_NAME);
        // requiredExts.push_back(XR_PICO_RESET_SENSOR_EXTENSION_NAME);
        return requiredExts;
    }

    void showPlatformInfo() {
        std::cout << "Platform Infos:" << std::endl;

        auto apiLayers = xr::enumerateApiLayerPropertiesToVector();
        uint32_t apiLayerCnt = apiLayers.size();

        std::cout << "Api Layers: " << apiLayerCnt << std::endl;
        for (const auto& apiLayer : apiLayers) {
            std::cout << fmt::format("{} v{}, spec: {}", apiLayer.layerName, apiLayer.layerVersion, to_string(apiLayer.specVersion)) << std::endl;
        }

        auto extProps = xr::enumerateInstanceExtensionPropertiesToVector(nullptr);
        uint32_t extPropCnt = extProps.size();

        std::cout << "exts info loaded" << std::endl;

        auto requiredExts = requiredExtensions();

        std::cout << "Extensions: " << extPropCnt << std::endl;
        for (const auto& extProp : extProps) {
            std::cout << fmt::format("{} v{}", extProp.extensionName, extProp.extensionVersion) << std::endl;

            if (extProp.extensionName == std::string(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME)) {
                extInfo.vive_tracker = true;
            }

            auto it = std::find_if(requiredExts.begin(), requiredExts.end(), [&](const char* extName) {
                return std::string(extName) == extProp.extensionName;
            });
            if (it != requiredExts.end()) {
                std::cout << "Extension " << *it << " is supported" << std::endl;
                requiredExts.erase(it);
            }
        }

        if (!requiredExts.empty()) {
            std::string s = "Error: Required Extension is not supported / ";
            for (const auto& ext : requiredExts) {
                s += ext;
                s += ", ";
            }
            s.pop_back();
            s.pop_back();
            throw std::runtime_error(s);
        }
        std::cout << "Required extensions all supported" << std::endl;
    }

    struct ExtInfo {
        bool vive_tracker = false;
    } extInfo;

    void* instanceCreateNext;

    xr::UniqueInstance instance;
    xr::SystemId systemId;
    std::unique_ptr<IGraphicsManager> graphicsManager;
    xr::UniqueSession session;

    std::vector<Swapchain> swapchains;
    xr::UniqueSpace appSpace;
    xr::UniqueSpace viewSpace;
    xr::UniqueSpace stageSpace;
    std::vector<XrView> views;

    xr::UniqueActionSet actionSet;
    struct HandActions {
        xr::UniqueAction pose;
        xr::UniqueAction trigger;
        xr::UniqueAction haptics;
        xr::Path subActionPath[2];
        xr::UniqueSpace space[2];
    } handActions;

    //xr::UniqueActionSet trackerActionSet;
    //struct TrackerActions {
    //    xr::Path subActionPath;
    //    xr::UniqueAction pose;
    //    xr::UniqueAction haptics;
    //    xr::UniqueSpace space;
    //} trackerActions;

    class HandVibrationProvider : public Game::IVibrationProvider {
        const int i;
        const App* app;
    public:
        HandVibrationProvider(int i, App* app) : i(i), app(app) {}
        void vibrate(float a) override {
            xr::HapticVibration vibration{};
            vibration.amplitude = a;
            vibration.duration = xr::Duration::minHaptic();
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

            xr::HapticActionInfo hapticActionInfo{};
            hapticActionInfo.action = app->handActions.haptics.get();
            hapticActionInfo.subactionPath = app->handActions.subActionPath[i];

            XR_CHK_ERR(app->session->applyHapticFeedback(hapticActionInfo, reinterpret_cast<XrHapticBaseHeader*>(&vibration)));
        }
    };
    std::optional<HandVibrationProvider> handVibProvider[2];

    xr::EventDataBuffer evBuf;
    bool shouldExit = false;
    bool session_running = false;

    Game::GameData gameData;

    void CreateInstance() {
        std::vector<const char*> layers = {};
        std::vector<const char*> exts = requiredExtensions();
//        if (extInfo.vive_tracker)
//            exts.push_back(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);

        xr::InstanceCreateInfo instanceCreateInfo{};
        instanceCreateInfo.next = instanceCreateNext;
        strcpy(instanceCreateInfo.applicationInfo.applicationName, "XRTest");
        strcpy(instanceCreateInfo.applicationInfo.engineName, "XRTest");
        instanceCreateInfo.applicationInfo.apiVersion = xr::Version::current();
        instanceCreateInfo.enabledApiLayerCount = layers.size();
        instanceCreateInfo.enabledApiLayerNames = layers.data();
        instanceCreateInfo.enabledExtensionCount = exts.size();
        instanceCreateInfo.enabledExtensionNames = exts.data();

        instance = xr::createInstanceUnique(instanceCreateInfo);
        if (instance == XR_NULL_HANDLE)
            throw std::runtime_error("instance creation error");
        std::cout << "instance creation succeeded" << std::endl;

        auto prop = instance->getInstanceProperties();
        std::cout << fmt::format("Runtime: {} v{}", prop.runtimeName, to_string(prop.runtimeVersion)) << std::endl;
    }

    void InitializeSystem() {
        xr::SystemGetInfo sysGetInfo{};
        sysGetInfo.formFactor = xr::FormFactor::HeadMountedDisplay;

        systemId = instance->getSystem(sysGetInfo);
        if (systemId == XR_NULL_SYSTEM_ID)
            throw std::runtime_error("system get error");

        auto props = instance->getSystemProperties(systemId);

        std::cout << fmt::format("SystemID: {:X}, FormFactor: {}", this->systemId.get(), to_string(sysGetInfo.formFactor)) << std::endl;
        std::cout << fmt::format("SystemName: {}, vendorID: {:X}", props.systemName, props.vendorId) << std::endl;
        std::cout << fmt::format("Max Layers: {}, Max Size: {}x{}", props.graphicsProperties.maxLayerCount, props.graphicsProperties.maxSwapchainImageWidth, props.graphicsProperties.maxSwapchainImageHeight) << std::endl;
        std::cout << fmt::format("Tracking: position/{}, orientation/{}", props.trackingProperties.positionTracking.get(), props.trackingProperties.orientationTracking.get()) << std::endl;
    }

    void InitializeSession() {
        auto graphicsBinding = graphicsManager->getXrGraphicsBinding();

        xr::SessionCreateInfo sessionCreateInfo{};
        sessionCreateInfo.systemId = systemId;
        sessionCreateInfo.next = graphicsBinding.get();

        session = instance->createSessionUnique(sessionCreateInfo);
        std::cout << "Session created" << std::endl;

        auto spaces = session->enumerateReferenceSpacesToVector();

        std::cout << "Reference spaces:" << std::endl;
        for (const auto space : spaces) {
            std::cout << to_string(space) << std::endl;
        }
    }

    void CreateReferenceSpace() {
        {
            xr::ReferenceSpaceCreateInfo createInfo;
            xr::Posef id{};
            id.orientation.w = 1;
            createInfo.poseInReferenceSpace = id;
            createInfo.referenceSpaceType = xr::ReferenceSpaceType::Local;

            appSpace = session->createReferenceSpaceUnique(createInfo);
        }
        {
            xr::ReferenceSpaceCreateInfo createInfo;
            xr::Posef id{};
            id.orientation.w = 1;
            createInfo.poseInReferenceSpace = id;
            createInfo.referenceSpaceType = xr::ReferenceSpaceType::View;

            viewSpace = session->createReferenceSpaceUnique(createInfo);
        }
        {
            xr::ReferenceSpaceCreateInfo createInfo;
            xr::Posef id{};
            id.orientation.w = 1;
            createInfo.poseInReferenceSpace = id;
//            createInfo.referenceSpaceType = xr::ReferenceSpaceType::Stage;

            // In PICO4, LOCAL is fixed (STAGE moves)
            createInfo.poseInReferenceSpace.position.y = -1.5;
            createInfo.referenceSpaceType = xr::ReferenceSpaceType::Local;

            stageSpace = session->createReferenceSpaceUnique(createInfo);
        }
    }

    void CreateSwapchain() {
        auto swapchainFmts = session->enumerateSwapchainFormatsToVector();
        auto swapchainFmtCnt = swapchainFmts.size();

        auto selectedSwapchainFmt = graphicsManager->chooseImageFormat(swapchainFmts);

        std::cout << "Avilable Swapchain Format x" << swapchainFmtCnt << std::endl;
        for (int i = 0; const auto & swapchainFmt : swapchainFmts) {
            std::cout << fmt::format("{}{}{}",
                swapchainFmt == selectedSwapchainFmt ? "[" : "",
                swapchainFmt,
                swapchainFmt == selectedSwapchainFmt ? "]" : "") << std::endl;
        }

        {
            auto configViews = instance->enumerateViewConfigurationsToVector(systemId);
            uint32_t viewCnt = configViews.size();

            std::cout << "ViewType x" << viewCnt << std::endl;
            for (int i = 0; const auto & configView : configViews) {
                auto blendModes = instance->enumerateEnvironmentBlendModesToVector(systemId, configView);
                uint32_t blendModeCnt = blendModes.size();

                std::cout << "ViewType " << i << std::endl;
                std::cout << "BlendMode x" << blendModeCnt << std::endl;
                for (const auto blendMode : blendModes) {
                    std::cout << to_string(blendMode) << std::endl;
                }
                i++;
            }
        }

        auto configViews = instance->enumerateViewConfigurationViewsToVector(systemId, xr::ViewConfigurationType::PrimaryStereo);
        auto viewCnt = configViews.size();

        this->views.resize(viewCnt);

        std::cout << "View x" << viewCnt << std::endl;

        for (int i = 0; const auto & configView : configViews) {
            std::cout << "View " << i << ":" << std::endl;
            std::cout << fmt::format("Size: typ/{}x{}, max/{}x{}",
                configView.recommendedImageRectWidth, configView.recommendedImageRectHeight,
                configView.maxImageRectWidth, configView.maxImageRectHeight) << std::endl;
            std::cout << fmt::format("Samples: typ/{}, max/{}",
                configView.recommendedSwapchainSampleCount, configView.maxSwapchainSampleCount) << std::endl;

            xr::SwapchainCreateInfo createInfo{};
            createInfo.arraySize = 1;
            createInfo.format = selectedSwapchainFmt;
            createInfo.width = configView.recommendedImageRectWidth;
            createInfo.height = configView.recommendedImageRectHeight;
            createInfo.mipCount = 1;
            createInfo.faceCount = 1;
            createInfo.sampleCount = configView.recommendedSwapchainSampleCount;
            createInfo.usageFlags = xr::SwapchainUsageFlagBits::ColorAttachment/* | xr::SwapchainUsageFlagBits::Sampled*/;

            Swapchain swapchain;
            swapchain.handle = session->createSwapchainUnique(createInfo);
            swapchain.extent.width = createInfo.width;
            swapchain.extent.height = createInfo.height;

            swapchains.emplace_back(std::move(swapchain));

            i++;
        }
        graphicsManager->InitializeRenderTargets(swapchains, selectedSwapchainFmt);
    }

    void InitializeAction() {
        handActions.subActionPath[0] = instance->stringToPath("/user/hand/left");
        handActions.subActionPath[1] = instance->stringToPath("/user/hand/right");
        //trackerActions.subActionPath = instance->stringToPath("/user/vive_tracker_htcx/role/chest");

        {
            xr::ActionSetCreateInfo createInfo{};
            strcpy(createInfo.actionSetName, "gameplay");
            strcpy(createInfo.localizedActionSetName, "Gameplay");
            createInfo.priority = 0;
            actionSet = instance->createActionSetUnique(createInfo);
        }

        //{
        //    xr::ActionSetCreateInfo createInfo{};
        //    strcpy(createInfo.actionSetName, "gameplay_track");
        //    strcpy(createInfo.localizedActionSetName, "Gameplay track");
        //    createInfo.priority = 0;
        //    trackerActionSet = instance->createActionSetUnique(createInfo);
        //}

        auto DefineAction = [&](std::string name, std::string lName, xr::ActionType type, xr::UniqueAction& action) {
            xr::ActionCreateInfo actionInfo{};
            actionInfo.actionType = type;
            strcpy(actionInfo.actionName, name.c_str());
            strcpy(actionInfo.localizedActionName, lName.c_str());
            actionInfo.countSubactionPaths = std::size(handActions.subActionPath);
            actionInfo.subactionPaths = handActions.subActionPath;
            action = actionSet->createActionUnique(actionInfo);
        };
        //auto DefineTrackerAction = [&](std::string name, std::string lName, xr::ActionType type, xr::UniqueAction& action) {
        //    xr::ActionCreateInfo actionInfo{};
        //    actionInfo.actionType = type;
        //    strcpy(actionInfo.actionName, name.c_str());
        //    strcpy(actionInfo.localizedActionName, lName.c_str());
        //    actionInfo.countSubactionPaths = 1;
        //    actionInfo.subactionPaths = &trackerActions.subActionPath;
        //    action = trackerActionSet->createActionUnique(actionInfo);
        //};
        auto ConfigAction = [&](std::string profile, std::initializer_list<std::pair<const xr::UniqueAction&, std::string>> bindingsDat) {
            auto profilePath = instance->stringToPath(profile.c_str());
            xr::InteractionProfileSuggestedBinding suggestedBindings{};

            std::vector<xr::ActionSuggestedBinding> bindings(bindingsDat.size());
            std::transform(bindingsDat.begin(), bindingsDat.end(), bindings.begin(), [&](const std::pair<const xr::UniqueAction&, std::string>& config) {
                auto path = instance->stringToPath(config.second.c_str());
                return xr::ActionSuggestedBinding{ config.first.get(), path};
                });
            suggestedBindings.interactionProfile = profilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = bindings.size();
            instance->suggestInteractionProfileBindings(suggestedBindings);
        };

        DefineAction("hand_pose", "Hand Pose", xr::ActionType::PoseInput, handActions.pose);
        DefineAction("trigger", "Trigger", xr::ActionType::BooleanInput, handActions.trigger);
        DefineAction("haptics", "Haptics", xr::ActionType::VibrationOutput, handActions.haptics);
        //DefineTrackerAction("tracker_pose", "Tracker Pose", xr::ActionType::PoseInput, trackerActions.pose);
        //DefineTrackerAction("tracker_haptics", "Tracker Haptics", xr::ActionType::VibrationOutput, trackerActions.haptics);

        ConfigAction("/interaction_profiles/khr/simple_controller", {
                { handActions.pose, "/user/hand/left/input/aim/pose" },
                { handActions.pose, "/user/hand/right/input/aim/pose" },
                { handActions.trigger, "/user/hand/left/input/select/click" },
                { handActions.trigger, "/user/hand/right/input/select/click" },
                { handActions.haptics, "/user/hand/left/output/haptic" },
                { handActions.haptics, "/user/hand/right/output/haptic" },
            });
        ConfigAction("/interaction_profiles/valve/index_controller", {
                { handActions.pose, "/user/hand/left/input/aim/pose" },
                { handActions.pose, "/user/hand/right/input/aim/pose" },
                { handActions.trigger, "/user/hand/left/input/trigger/click" },
                { handActions.trigger, "/user/hand/right/input/trigger/click" },
                { handActions.haptics, "/user/hand/left/output/haptic" },
                { handActions.haptics, "/user/hand/right/output/haptic" },
            });
//        ConfigAction("/interaction_profiles/google/daydream_controller", {
//                { handActions.pose, "/user/hand/left/input/aim/pose" },
//                { handActions.pose, "/user/hand/right/input/aim/pose" },
//                { handActions.trigger, "/user/hand/left/input/select/click" },
//                { handActions.trigger, "/user/hand/right/input/select/click" },
//            });
        ConfigAction("/interaction_profiles/htc/vive_controller", {
                { handActions.pose, "/user/hand/left/input/aim/pose" },
                { handActions.pose, "/user/hand/right/input/aim/pose" },
                { handActions.trigger, "/user/hand/left/input/trigger/click" },
                { handActions.trigger, "/user/hand/right/input/trigger/click" },
                { handActions.haptics, "/user/hand/left/output/haptic" },
                { handActions.haptics, "/user/hand/right/output/haptic" },
            });
        ConfigAction("/interaction_profiles/microsoft/motion_controller", {
                { handActions.pose, "/user/hand/left/input/aim/pose" },
                { handActions.pose, "/user/hand/right/input/aim/pose" },
                { handActions.trigger, "/user/hand/left/input/squeeze/click" },
                { handActions.trigger, "/user/hand/right/input/squeeze/click" },
                { handActions.haptics, "/user/hand/left/output/haptic" },
                { handActions.haptics, "/user/hand/right/output/haptic" },
            });
//        ConfigAction("/interaction_profiles/oculus/go_controller", {
//                { handActions.pose, "/user/hand/left/input/aim/pose" },
//                { handActions.pose, "/user/hand/right/input/aim/pose" },
//                { handActions.trigger, "/user/hand/left/input/trigger/click" },
//                { handActions.trigger, "/user/hand/right/input/trigger/click" },
//            });
        ConfigAction("/interaction_profiles/oculus/touch_controller", {
                { handActions.pose, "/user/hand/left/input/aim/pose" },
                { handActions.pose, "/user/hand/right/input/aim/pose" },
                { handActions.trigger, "/user/hand/left/input/trigger/touch" },
                { handActions.trigger, "/user/hand/right/input/trigger/touch" },
                { handActions.haptics, "/user/hand/left/output/haptic" },
                { handActions.haptics, "/user/hand/right/output/haptic" },
            });
//        ConfigAction("/interaction_profiles/pico/neo3_controller", {
//                { handActions.pose, "/user/hand/left/input/aim/pose" },
//                { handActions.pose, "/user/hand/right/input/aim/pose" },
//                { handActions.trigger, "/user/hand/left/input/trigger/value" },
//                { handActions.trigger, "/user/hand/right/input/trigger/value" },
//                { handActions.haptics, "/user/hand/left/output/haptic" },
//                { handActions.haptics, "/user/hand/right/output/haptic" },
//            });
        //ConfigAction("/interaction_profiles/htc/vive_tracker_htcx", {
        //        { trackerActions.pose, "/user/vive_tracker_htcx/role/chest/input/grip/pose" },
        //        { trackerActions.haptics, "/user/vive_tracker_htcx/role/chest/output/haptic" },
        //    });

        for(int i = 0; i < 2; i++)
        {
            xr::ActionSpaceCreateInfo actionSpaceInfo{};
            actionSpaceInfo.action = handActions.pose.get();
            actionSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
            actionSpaceInfo.subactionPath = handActions.subActionPath[i];
            handActions.space[i] = session->createActionSpaceUnique(actionSpaceInfo);
        }

        //{
        //    xr::ActionSpaceCreateInfo actionSpaceInfo{};
        //    actionSpaceInfo.action = trackerActions.pose.get();
        //    actionSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
        //    actionSpaceInfo.subactionPath = trackerActions.subActionPath;
        //    trackerActions.space = session->createActionSpaceUnique(actionSpaceInfo);
        //}

        auto actionSets = { actionSet.get(), /*trackerActionSet.get()*/ };

        xr::SessionActionSetsAttachInfo attachInfo{};
        attachInfo.countActionSets = actionSets.size();
        attachInfo.actionSets = actionSets.begin();
        session->attachSessionActionSets(attachInfo);

        for (int i = 0; i < 2; i++) {
            handVibProvider[i].emplace(i, this);
            gameData.handVib[i].emplace(std::ref<Game::IVibrationProvider>(handVibProvider[i].value()));
        }
    }

    void HandleSessionStateChange(const xr::EventDataSessionStateChanged& ev) {
        if (ev.session != this->session.get()) {
            std::cout << "Event from Unknown Session" << std::endl;
            return;
        }
        switch (ev.state)
        {
        case xr::SessionState::Ready:
        {
            xr::SessionBeginInfo beginInfo;
            beginInfo.primaryViewConfigurationType = xr::ViewConfigurationType::PrimaryStereo;
            session->beginSession(beginInfo);
            session_running = true;
            std::cout << "session began" << std::endl;
            break;
        }
        case xr::SessionState::Stopping:
        {
            session->endSession();
            session_running = false;
            std::cout << "session ended" << std::endl;
            break;
        }
        case xr::SessionState::Exiting:
        case xr::SessionState::LossPending:
            //session_running = false;
            shouldExit = true;
            break;
        default:
            break;
        }
    }

    bool PollOneEvent() {
        auto result = instance->pollEvent(evBuf);
        switch (result)
        {
        case xr::Result::Success:
            if (this->evBuf.type == xr::StructureType::EventDataEventsLost) {
                auto& eventsLost = *reinterpret_cast<xr::EventDataEventsLost*>(&this->evBuf);
                std::cout << "Event Lost: " << eventsLost.lostEventCount << std::endl;
            }
            return true;
        case xr::Result::EventUnavailable:
            return false;
        default:
            throw std::runtime_error(fmt::format("failed to poll event: {}", to_string(result)));
        }
    }

    void PollEvent() {
        while (PollOneEvent()) {
            std::cout << fmt::format("Event: {}({})", to_string(this->evBuf.type), (int)this->evBuf.type) << std::endl;
            switch (this->evBuf.type)
            {
            case xr::StructureType::EventDataSessionStateChanged:
            {
                const auto& ev = *reinterpret_cast<xr::EventDataSessionStateChanged*>(&this->evBuf);
                std::cout << "State -> " << to_string(ev.state) << std::endl;
                HandleSessionStateChange(ev);
                break;
            }
            case xr::StructureType::EventDataViveTrackerConnectedHTCX: {
                const auto& viveTrackerConnected =
                    *reinterpret_cast<xr::EventDataViveTrackerConnectedHTCX*>(&this->evBuf);
                std::cout << instance->pathToString(viveTrackerConnected.paths->persistentPath) << std::endl;
                std::cout << instance->pathToString(viveTrackerConnected.paths->rolePath) << std::endl;
                break;
            }
            default:
                break;
            }

        }
    }

    void PollAction() {
        xr::ActiveActionSet activeActionSet;
        activeActionSet.actionSet = actionSet.get();
        activeActionSet.subactionPath = xr::Path(XR_NULL_PATH);

        //xr::ActiveActionSet activeActionSet2;
        //activeActionSet2.actionSet = trackerActionSet.get();
        //activeActionSet2.subactionPath = xr::Path(XR_NULL_PATH);

        auto activeActionSets = { activeActionSet, /*activeActionSet2*/ };

        xr::ActionsSyncInfo syncInfo{};
        syncInfo.countActiveActionSets = activeActionSets.size();
        syncInfo.activeActionSets = activeActionSets.begin();
        XR_CHK_ERR(session->syncActions(syncInfo));

        static bool oldState[2] = {};

        for (int i = 0; i < 2; i++) {
            xr::ActionStateGetInfo getInfo{};
            getInfo.action = handActions.trigger.get();
            getInfo.subactionPath = handActions.subActionPath[i];
            auto trigState = session->getActionStateBoolean(getInfo);

            if (trigState.isActive && trigState.currentState && !oldState[i]) {
                
                //xr::HapticActionInfo hapticActionInfo2{};
                //hapticActionInfo2.action = trackerActions.haptics.get();
                //hapticActionInfo2.subactionPath = trackerActions.subActionPath;

                //std::cout << instance->pathToString(trackerActions.subActionPath) << std::endl;

                //XR_CHK_ERR(session->applyHapticFeedback(hapticActionInfo2, reinterpret_cast<XrHapticBaseHeader*>(&vibration)));

                gameData.trigger[i] = true;
            }
            else {
                gameData.trigger[i] = false;
            }
            oldState[i] = trigState.currentState && trigState.isActive;
        }

        for(int i = 0; i < 2; i++)
        {
            xr::ActionStateGetInfo getInfo{};
            getInfo.action = handActions.pose.get();
            getInfo.subactionPath = handActions.subActionPath[i];
            auto poseState = session->getActionStatePose(getInfo);
        }
        //{
        //    xr::ActionStateGetInfo getInfo{};
        //    getInfo.action = trackerActions.pose.get();
        //    getInfo.subactionPath = trackerActions.subActionPath;
        //    auto poseState = session->getActionStatePose(getInfo);
        //}
    }

    void RenderFrame() {
        xr::FrameWaitInfo frameWaitInfo;
        auto frameState = session->waitFrame(frameWaitInfo);

        xr::FrameBeginInfo beginInfo{};
        XR_CHK_ERR(session->beginFrame(beginInfo));

        constexpr auto max_layers_num = 1;
        constexpr auto max_views_num = 2;

        std::array<xr::CompositionLayerBaseHeader*, max_layers_num> layers{};
        xr::CompositionLayerProjection layer{};
        std::array<xr::CompositionLayerProjectionView, max_views_num> projectionViews{};

        xr::FrameEndInfo endInfo;
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = xr::EnvironmentBlendMode::Opaque;
        endInfo.layerCount = 0;
        endInfo.layers = layers.data();

        if (frameState.shouldRender) {
            xr::ViewLocateInfo locateInfo;
            locateInfo.viewConfigurationType = xr::ViewConfigurationType::PrimaryStereo;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = this->appSpace.get();

            XrViewState viewState{ XR_TYPE_VIEW_STATE };
            uint32_t viewCountOutput;

            XR_CHK_ERR(session->locateViews(locateInfo, &viewState, this->views.size(), &viewCountOutput, views.data()));
            if (viewCountOutput != views.size())
                throw std::runtime_error("Failed to locate views: viewCountOutput != views.size()");

            for (int i = 0; i < 2; i++) {
                gameData.handPoses[i] = SpaceToPose(handActions.space[i].get(), appSpace.get(), frameState.predictedDisplayTime);
            }

            gameData.viewPose = SpaceToPose(viewSpace.get(), appSpace.get(), frameState.predictedDisplayTime);
            gameData.stagePose = SpaceToPose(stageSpace.get(), appSpace.get(), frameState.predictedDisplayTime);

            //renderDat.track = SpaceToPose(trackerActions.space.get(), appSpace.get(), frameState.predictedDisplayTime);

            gameData.dt = (long double)(frameState.predictedDisplayPeriod.get()) / 1'000'000'000;

            Game::proc(gameData);

            for (uint32_t i = 0; const auto & _ : swapchains) {
                const auto& swapchain = swapchains[i].handle;

                xr::SwapchainImageAcquireInfo acquireInfo;
                auto imageIndex = swapchain->acquireSwapchainImage(acquireInfo);

                xr::SwapchainImageWaitInfo waitInfo;
                waitInfo.timeout = xr::Duration::infinite();
                swapchain->waitSwapchainImage(waitInfo);

                projectionViews[i].type = xr::StructureType::CompositionLayerProjectionView;
                projectionViews[i].pose = views[i].pose;
                projectionViews[i].fov = views[i].fov;
                projectionViews[i].subImage.swapchain = swapchain.get();
                projectionViews[i].subImage.imageRect.offset = xr::Offset2Di{ 0, 0 };
                projectionViews[i].subImage.imageRect.extent = swapchains[i].extent;

                graphicsManager->render(i, imageIndex, projectionViews[i]);

                xr::SwapchainImageReleaseInfo releaseInfo;
                swapchain->releaseSwapchainImage(releaseInfo);

                i++;
            }

            layer.space = appSpace.get();
            layer.layerFlags = {};
            layer.viewCount = swapchains.size();
            layer.views = projectionViews.data();
            layers[endInfo.layerCount++] = reinterpret_cast<xr::CompositionLayerBaseHeader*>(&layer);
        }

        session->endFrame(endInfo);
    }

    int cnt = 0;

    bool* pResumed = nullptr;
    android_app* pAndroidApp = nullptr;

public:
    void setAndroidSettings(bool* _pResumed, android_app* _pAndroidApp){
        pResumed = _pResumed;
        pAndroidApp = _pAndroidApp;
    }

    App(void* instanceCreateNext = nullptr) : instanceCreateNext(instanceCreateNext) {
        showPlatformInfo();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "aa");
        CreateInstance();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "bb");
        InitializeSystem();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "cc");
        graphicsManager = CreateGraphicsManager_Vulkan(instance.get(), systemId);
        __android_log_write(ANDROID_LOG_INFO, "mylog", "dd");

        InitializeSession();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "ee");
        InitializeAction();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "ff");
        CreateSwapchain();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "gg");

        CreateReferenceSpace();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "hh");

        graphicsManager->PrepareResources();
        __android_log_write(ANDROID_LOG_INFO, "mylog", "ii");
    }

    void MainLoop() {
        while (!shouldExit) {
#ifdef XR_USE_PLATFORM_ANDROID
            while(1) {
                int events;
                struct android_poll_source* source;
                // If the timeout is zero, returns immediately without blocking.
                // If the timeout is negative, waits indefinitely until an event appears.
                const int timeoutMilliseconds =
                    ((pResumed != nullptr) && !(*pResumed) && !session_running) ? -1 : 0;
                if (ALooper_pollAll(timeoutMilliseconds, nullptr, &events, (void**)&source) < 0) {
                    break;
                }

                // Process this event.
                if (source != nullptr) {
                    source->process(pAndroidApp, source);
                }
            }
#endif
            PollEvent();
            if (session_running) {
                PollAction();
                RenderFrame();
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    }

    ~App() {
        std::cout << "Session destroyed" << std::endl;
    }
};

#ifdef XR_USE_PLATFORM_ANDROID

struct AndroidAppState {
    ANativeWindow* NativeWindow = nullptr;
    bool Resumed = false;
};

/**
 * Process the next main command.
 */
static void app_handle_cmd(struct android_app* app, int32_t cmd) {
    AndroidAppState* appState = (AndroidAppState*)app->userData;

    switch (cmd) {
        // There is no APP_CMD_CREATE. The ANativeActivity creates the
        // application thread from onCreate(). The application thread
        // then calls android_main().
        case APP_CMD_START: {
//            Log::Write(Log::Level::Info, "    APP_CMD_START");
//            Log::Write(Log::Level::Info, "onStart()");
            break;
        }
        case APP_CMD_RESUME: {
//            Log::Write(Log::Level::Info, "onResume()");
//            Log::Write(Log::Level::Info, "    APP_CMD_RESUME");
            appState->Resumed = true;
//            pxr::Pxr_SetEngineVersion("2.8.0.1");
//            pxr::Pxr_StartCVControllerThread(PXR_HMD_6DOF, PXR_CONTROLLER_6DOF);
            break;
        }
        case APP_CMD_PAUSE: {
//            Log::Write(Log::Level::Info, "onPause()");
//            Log::Write(Log::Level::Info, "    APP_CMD_PAUSE");
            appState->Resumed = false;
//            pxr::Pxr_SetEngineVersion("2.7.0.0");
//            pxr::Pxr_StopCVControllerThread(PXR_HMD_6DOF, PXR_CONTROLLER_6DOF);
            break;
        }
        case APP_CMD_STOP: {
//            Log::Write(Log::Level::Info, "onStop()");
//            Log::Write(Log::Level::Info, "    APP_CMD_STOP");
            break;
        }
        case APP_CMD_DESTROY: {
//            Log::Write(Log::Level::Info, "onDestroy()");
//            Log::Write(Log::Level::Info, "    APP_CMD_DESTROY");
            appState->NativeWindow = NULL;
            break;
        }
        case APP_CMD_INIT_WINDOW: {
//            Log::Write(Log::Level::Info, "surfaceCreated()");
//            Log::Write(Log::Level::Info, "    APP_CMD_INIT_WINDOW");
            appState->NativeWindow = app->window;
            break;
        }
        case APP_CMD_TERM_WINDOW: {
//            Log::Write(Log::Level::Info, "surfaceDestroyed()");
//            Log::Write(Log::Level::Info, "    APP_CMD_TERM_WINDOW");
            appState->NativeWindow = NULL;
            break;
        }
    }
}

static int32_t onInputEvent(struct android_app* app, AInputEvent* event){
    int type = AInputEvent_getType(event);
    if(type == AINPUT_EVENT_TYPE_KEY){
        int32_t action = AKeyEvent_getAction(event);
        int32_t code   = AKeyEvent_getKeyCode(event);
        //Log::Write(Log::Level::Error, Fmt("xxxx:%d:%d\n", code, action));
        //if(code == 4)  return 1;
    }
    return 0;
}


aaudio_data_callback_result_t myCallback(
        AAudioStream *stream,
        void *userData,
        void *audioData,
        int32_t numFrames) {
    int64_t timeout = 0;

    static float phase = 0;

    auto& cnt = *reinterpret_cast<int*>(userData);

    // Write samples directly into the audioData array.
    auto pDat = reinterpret_cast<float*>(audioData);
    for(int i = 0; i < numFrames; i++){
        pDat[i * 2] = 0.3 * sin(phase);
        pDat[i * 2 + 1] = 0.1 * sin(phase);

        phase += 0.1;
        if(phase > 3.141592 * 2) phase -= 3.141592 * 2;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
void android_main(struct android_app* app) {
    androidbuf abuf;
    std::cout.rdbuf(&abuf);
    try {
        JNIEnv* Env;
        app->activity->vm->AttachCurrentThread(&Env, nullptr);

        AndroidAppState appState = {};

        app->userData = &appState;
        app->onAppCmd = app_handle_cmd;
        app->onInputEvent = onInputEvent;

        asset_manager = app->activity->assetManager;

        int cnt = 0;

//        AAudioStreamBuilder *aaudioBuilder;
//        AAudioStream *aaudioStream;
//        {
//            auto res = AAudio_createStreamBuilder(&aaudioBuilder);
//            std::cout << "AAudio_createStreamBuilder result: " << res << std::endl;
//        }
//        {
//            AAudioStreamBuilder_setFormat(aaudioBuilder, AAUDIO_FORMAT_PCM_FLOAT);
//            AAudioStreamBuilder_setDataCallback(aaudioBuilder, myCallback, &cnt);
//            AAudioStreamBuilder_setPerformanceMode(aaudioBuilder, AAUDIO_PERFORMANCE_MODE_NONE);
//        }
//        {
//            auto res = AAudioStreamBuilder_openStream(aaudioBuilder, &aaudioStream);
//            std::cout << "AAudioStreamBuilder_openStream result: " << res << std::endl;
//        }
//        {
//            std::cout << "AAudioStream_getDeviceId(): " << AAudioStream_getDeviceId(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getDirection(): " << AAudioStream_getDirection(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getSharingMode(): " << AAudioStream_getSharingMode(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getSampleRate(): " << AAudioStream_getSampleRate(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getChannelCount(): " << AAudioStream_getChannelCount(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getFormat(): " << AAudioStream_getFormat(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getBufferSizeInFrames(): " << AAudioStream_getBufferSizeInFrames(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getBufferCapacityInFrames(): " << AAudioStream_getBufferCapacityInFrames(aaudioStream) << std::endl;
//            std::cout << "AAudioStream_getFramesPerBurst(): " << AAudioStream_getFramesPerBurst(aaudioStream) << std::endl;
//        }
//        {
//            AAudioStream_setBufferSizeInFrames(aaudioStream, AAudioStream_getFramesPerBurst(aaudioStream) * 2);
//        }
//        {
//            auto res = AAudioStream_requestStart(aaudioStream);
//            std::cout << "AAudioStream_requestStart result: " << res << std::endl;
//        }

        {
            XrLoaderInitInfoAndroidKHR loaderInitInfoAndroid{};
            loaderInitInfoAndroid.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
            loaderInitInfoAndroid.next = NULL;
            loaderInitInfoAndroid.applicationVM = app->activity->vm;
            loaderInitInfoAndroid.applicationContext = app->activity->clazz;
            xr::initializeLoaderKHR(
                    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfoAndroid),
                    xr::DispatchLoaderDynamic());
        }

        XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroid{};
        instanceCreateInfoAndroid.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
        instanceCreateInfoAndroid.applicationVM = app->activity->vm;
        instanceCreateInfoAndroid.applicationActivity = app->activity->clazz;

        App myapp{&instanceCreateInfoAndroid};
        myapp.setAndroidSettings(&(appState.Resumed), app);

        myapp.MainLoop();


//        {
//            auto res = AAudioStream_close(aaudioStream);
//            std::cout << "AAudioStream_close result: " << res << std::endl;
//        }
//        {
//            auto res = AAudioStreamBuilder_delete(aaudioBuilder);
//            std::cout << "AAudioStreamBuilder_delete result: " << res << std::endl;
//        }

        app->activity->vm->DetachCurrentThread();
    } catch (const std::exception& ex) {
        Log::Write(Log::Level::Error, ex.what());
    } catch (...) {
        Log::Write(Log::Level::Error, "Unknown Error");
    }
}
#endif
