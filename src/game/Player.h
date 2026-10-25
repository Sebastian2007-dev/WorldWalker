#pragma once
#include <glm/glm.hpp>

#include <vector>

#include "world/Vegetation.h"
#include "world/WorldData.h"

namespace game {

struct InputState {
    bool forward = false, back = false, left = false, right = false;
    bool run = false, jump = false, up = false, down = false;
    bool kneel = false;   // toggle request (pressed this frame)
    glm::vec2 mouseDelta{0};
    float scroll = 0;
};

class Player {
public:
    enum class View { FirstPerson, ThirdPerson };

    void spawn(double lat, double lon, world::WorldData& data);
    void setLook(float yawDeg, float pitchDeg) { yaw_ = yawDeg * 0.0174533f; pitch_ = pitchDeg * 0.0174533f; }
    void update(float dt, const InputState& in, world::WorldData& data, world::Vegetation& veg);
    void previewCamera(float distance, float height, float horizontalOffset);

    // Camera (ECEF position, forward/up directions in ECEF axes).
    glm::dvec3 cameraPos() const { return camPos_; }
    glm::vec3 cameraForward() const { return camFwd_; }
    glm::vec3 cameraUp() const { return glm::vec3(up_); }

    glm::dvec3 position() const { return pos_; }
    double lat() const { return lat_; }
    double lon() const { return lon_; }
    double altitude() const { return h_; }
    double groundHeight() const { return ground_; }
    float heading() const { return yaw_; }            // where the camera looks (compass)
    float lookPitch() const { return pitch_; }
    float bodyHeading() const { return bodyYaw_; }     // where the character faces
    float bodyTurnRate() const { return turnRate_; }   // rad/s, positive = turning right
    bool kneeling() const { return kneeling_; }
    float speed() const { return speed_; }
    float walkPhase() const { return walkPhase_; }
    // For the character animation: ground velocity in the body frame (x right, y forward, m/s), whether the
    // feet touch the ground, vertical speed, and whether a jump started during the last update.
    glm::vec2 localVelocity() const { return localVel_; }
    bool onGround() const { return onGround_ && !flying; }
    float verticalSpeed() const { return vUp_; }
    bool jumped() const { return jumped_; }
    bool waitingForGround() const { return waitingForGround_; }
    glm::dvec3 east() const { return east_; }
    glm::dvec3 north() const { return north_; }
    glm::dvec3 up() const { return up_; }

    View view = View::FirstPerson;
    bool flying = false;
    float flySpeed = 50.0f;
    float thirdPersonDistance = 4.5f;
    float mouseSensitivity = 0.0025f;

private:
    void refreshFrame();
    void updateCamera(world::WorldData& data);

    glm::dvec3 pos_{0}, camPos_{0};
    glm::dvec3 east_{1, 0, 0}, north_{0, 1, 0}, up_{0, 0, 1};
    glm::vec3 camFwd_{0, 1, 0};
    double lat_ = 0, lon_ = 0, h_ = 0, ground_ = 0;
    float yaw_ = 0, pitch_ = 0;           // yaw: radians clockwise from north
    float bodyYaw_ = 0, turnRate_ = 0;
    bool turningInPlace_ = false;
    bool kneeling_ = false;
    float eyeHeight_ = 1.70f;             // follows kneeling smoothly
    float vUp_ = 0;                       // vertical velocity
    bool onGround_ = false;
    bool waitingForGround_ = false;
    float speed_ = 0, walkPhase_ = 0;
    glm::vec2 localVel_{0};
    bool jumped_ = false;
    std::vector<world::Trunk> trunks_;
    float trunkTimer_ = 0;
};

}  // namespace game
