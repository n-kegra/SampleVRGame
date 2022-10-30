#ifndef GAME_H
#define GAME_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "GraphicsProvider.hpp"
#include <optional>

namespace Game {

class IVibrationProvider {
public:
	virtual void vibrate(float a) = 0;
};

struct Pose {
	glm::vec3 pos;
	glm::quat ori;
};

struct GameData {
	double dt;

	std::optional<Pose> viewPose;
	std::optional<Pose> stagePose;
	std::optional<Pose> handPoses[2];
	bool trigger[2];

	std::optional<std::reference_wrapper<IVibrationProvider>> handVib[2];
};

void init(IGraphicsProvider& g);

void proc(const GameData& dat);

void draw(IGraphicsProvider& g);

}

#endif
