#pragma once

#include <filesystem>
#include <glm/glm.hpp>

using ModelHandle = int;

class IGraphicsProvider {
public:
	virtual ModelHandle LoadModel(const char* path) = 0;
	virtual void DrawModel(ModelHandle model, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale, const glm::mat4& mat = glm::identity<glm::mat4>()) = 0;
};
