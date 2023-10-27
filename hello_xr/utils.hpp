#pragma once

#include <filesystem>
#include <fstream>
#include <exception>
#ifdef XR_USE_PLATFORM_ANDROID
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#endif

extern AAssetManager* asset_manager;

inline auto file_get_contents(std::filesystem::path path) {
#ifdef XR_USE_PLATFORM_ANDROID
    auto path_str = path.string();
    auto asset = AAssetManager_open(asset_manager,  path_str.c_str(), AASSET_MODE_BUFFER);
    if(asset == NULL) {
        std::cout << "file load error: " << path << std::endl;
        throw std::runtime_error("file load error");
    }
    size_t fileSz = AAsset_getLength(asset);
    std::vector<std::byte> data(fileSz);
    AAsset_read(asset, data.data(), fileSz);
    AAsset_close(asset);
    return data;
#else
    size_t fileSz = std::filesystem::file_size(path);
    std::ifstream file{ path, std::ios_base::binary };
    std::vector<std::byte> data(fileSz);
    file.read(reinterpret_cast<char*>(data.data()), fileSz);
    return data;
#endif
}
