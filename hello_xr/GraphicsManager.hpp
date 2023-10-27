#ifndef GRAPHICS_MANAGER_HPP
#define GRAPHICS_MANAGER_HPP

#include <memory>
#include <optional>

#include <openxr/openxr.hpp>

struct Swapchain {
	xr::UniqueSwapchain handle;
	xr::Extent2Di extent;
};

class IGraphicsManager {
public:
	virtual std::unique_ptr<xr::impl::InputStructBase> getXrGraphicsBinding() const = 0;
	virtual int64_t chooseImageFormat(const std::vector<int64_t>& formats) const = 0;
	virtual void InitializeRenderTargets(const std::vector<Swapchain>& swapchains, int64_t format) = 0;
	virtual void PrepareResources() = 0;
	virtual void render(int viewIndex, int imageIndex, const xr::CompositionLayerProjectionView& view) = 0;
};

#endif
