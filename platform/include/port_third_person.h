#pragma once

#include <algorithm>
#include <cmath>

class CStateManager;
class CTransform4f;

// An opt-in third-person gameplay camera: while Samus stands unmorphed under the first-person
// camera, the world is drawn from behind her shoulder, her body is shown and the arm cannon
// and helmet are left out. Morph ball, cutscenes, path/spindle cameras and the free camera
// keep their own views. Setting `third_person` (F1 > Video, console `thirdperson`).
namespace PortThirdPerson {

struct Params {
  float distance = 3.0f; // metres behind the eye
  float shoulder = 0.6f; // metres to the right
  float height = 0.25f;  // metres above the first-person eye
};
constexpr float kDistanceMin = 0.5f, kDistanceMax = 10.f;
constexpr float kShoulderMin = -2.f, kShoulderMax = 2.f;
constexpr float kHeightMin = -1.f, kHeightMax = 2.f;
constexpr float kWallMargin = 0.2f; // metres kept between the camera and the wall

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};

// Pure maths (no game state): where the camera wants to be, and where it ends up when a wall
// is hit `hitDistance` along the line from the pivot to it (negative: no hit).
inline Vec3 ComputeEye(const Vec3& eye, const Vec3& forward, const Vec3& right, const Vec3& up,
                       const Params& p) {
  return {eye.x + right.x * p.shoulder + up.x * p.height - forward.x * p.distance,
          eye.y + right.y * p.shoulder + up.y * p.height - forward.y * p.distance,
          eye.z + right.z * p.shoulder + up.z * p.height - forward.z * p.distance};
}

// How far along the pivot->desired line the camera may go.
inline float PullInLength(float fullLength, float hitDistance, float margin = kWallMargin) {
  if (hitDistance >= 0.f && hitDistance < fullLength) {
    return std::max(0.f, hitDistance - margin);
  }
  return fullLength;
}

inline Vec3 PullIn(const Vec3& pivot, const Vec3& desired, float hitDistance,
                   float margin = kWallMargin) {
  const Vec3 d{desired.x - pivot.x, desired.y - pivot.y, desired.z - pivot.z};
  const float full = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(full > 0.f)) {
    return desired;
  }
  const float len = PullInLength(full, hitDistance, margin);
  if (len >= full) {
    return desired;
  }
  const float s = len / full;
  return {pivot.x + d.x * s, pivot.y + d.y * s, pivot.z + d.z * s};
}

bool Enabled();
void SetEnabled(bool on);
// Third person is drawing this tick (enabled, and the camera and player are in a state it takes).
bool Active();

Params GetParams();
void SetDistance(float metres);
void SetShoulder(float metres);
void SetHeight(float metres);

// Once per game tick, after the camera manager's update.
void Update(CStateManager& mgr, float dt);
// The camera's tick position (for the console); valid while Active().
void GetCameraPosition(float out[3]);
// The view the world is drawn from: the first-person view moved back to the blended,
// wall-clamped distance; `game` when not active.
CTransform4f View(const CTransform4f& game);

} // namespace PortThirdPerson
