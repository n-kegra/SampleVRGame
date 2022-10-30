#pragma once

#include "GraphicsManager.hpp"

std::unique_ptr<IGraphicsManager> CreateGraphicsManager_Vulkan(xr::Instance instance, xr::SystemId systemId);
std::vector<const char*> GetGraphicsExtension_Vulkan();
