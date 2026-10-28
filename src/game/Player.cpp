#include "game/Player.h"

#include <algorithm>
#include <cmath>

#include "geo/Geodesy.h"

namespace game {
namespace {
constexpr float kEyeHeight = 1.70f;
constexpr float kWalkSpeed = 1.6f;      // m/s, a brisk walk
constexpr float kRunSpeed = 6.0f;
constexpr float kJumpSpeed = 4.2f;
constexpr float kGravity = 9.81f;
constexpr float kBodyRadius = 0.3f;
}  // namespace

void Player::spawn(double lat, double lon, world::WorldData& data) {
    lat_ = lat;
    lon_ = lon;
    vUp_ = 0;
    waitingForGround_ = !data.tryMeshHeight(lat, lon, ground_);
    if (waitingForGround_) ground_ = 0;
    h_ = ground_;
    pos_ = geo::toEcef(lat_, lon_, h_);
    refreshFrame();
    updateCamera(data);
}

void Player::refreshFrame() {
    geo::Enu e = geo::enuFrame(lat_, lon_);
    east_ = e.east;
    north_ = e.north;
    up_ = e.up;
}

void Player::update(float dt, const InputState& in, world::WorldData& data, world::Vegetation& veg) {
    dt = std::min(dt, 0.1f);
    jumped_ = false;
    localVel_ = glm::vec2(0.0f);
    yaw_ += in.mouseDelta.x * mouseSensitivity;
    pitch_ = std::clamp(pitch_ - in.mouseDelta.y * mouseSensitivity, -1.5f, 1.5f);
    yaw_ = std::fmod(yaw_ + 6.2831853f, 6.2831853f);
    if (in.kneel && !flying) kneeling_ = !kneeling_;
    bool moving = in.forward || in.back || in.left || in.right || in.jump;
    if (moving || flying) kneeling_ = false;

    if (view == View::ThirdPerson && !flying)
        thirdPersonDistance = std::clamp(thirdPersonDistance * std::pow(0.9f, in.scroll), 1.5f, 30.0f);
    if (flying) flySpeed = std::clamp(flySpeed * std::pow(1.25f, in.scroll), 2.0f, 50000.0f);

    glm::dvec3 fwdH = north_ * double(std::cos(yaw_)) + east_ * double(std::sin(yaw_));
    glm::dvec3 rightH = east_ * double(std::cos(yaw_)) - north_ * double(std::sin(yaw_));
    glm::dvec3 wish{0};
    if (in.forward) wish += fwdH;
    if (in.back) wish -= fwdH;
    if (in.right) wish += rightH;
    if (in.left) wish -= rightH;
    if (glm::length(wish) > 0) wish = glm::normalize(wish);
    if (kneeling_) wish = glm::dvec3(0.0);

    // Body yaw: while moving or flying it turns smoothly towards the view; standing still it stays put until the
    // view is more than 75 degrees away, then turns in place (with the turn animation) until it faces the view. In
    // third person it stays put however far the view is, so the camera can circle the standing character.
    {
        auto wrap = [](float a) { return std::remainder(a, 6.2831853f); };
        float diff = wrap(yaw_ - bodyYaw_);
        float before = bodyYaw_;
        if (dt > 0.0f) {
            if (moving || flying) {
                turningInPlace_ = false;
                bodyYaw_ += diff * (1.0f - std::exp(-dt * 10.0f));
            } else if (view == View::ThirdPerson) {
                turningInPlace_ = false;
            } else if (!kneeling_) {
                if (std::abs(diff) > 1.3f) turningInPlace_ = true;
                if (turningInPlace_) {
                    float step = std::min(std::abs(diff), dt * 3.0f);
                    bodyYaw_ += diff > 0.0f ? step : -step;
                    if (std::abs(diff) < 0.05f) turningInPlace_ = false;
                }
            }
            if (view == View::FirstPerson && !moving && !flying && std::abs(diff) > 1.3f)
                bodyYaw_ = yaw_ - (diff > 0.0f ? 1.3f : -1.3f);  // own body never ends up behind the camera
            bodyYaw_ = std::fmod(bodyYaw_ + 6.2831853f, 6.2831853f);
            turnRate_ = wrap(bodyYaw_ - before) / dt;
        }
    }
    float eyeTarget = kneeling_ ? 1.12f : kEyeHeight;
    eyeHeight_ += (eyeTarget - eyeHeight_) * (1.0f - std::exp(-dt * 4.0f));

    if (flying) {
        glm::dvec3 look = fwdH * double(std::cos(pitch_)) + up_ * double(std::sin(pitch_));
        glm::dvec3 move{0};
        if (in.forward) move += look;
        if (in.back) move -= look;
        if (in.right) move += rightH;
        if (in.left) move -= rightH;
        if (in.up || in.jump) move += up_;
        if (in.down) move -= up_;
        if (glm::length(move) > 0) move = glm::normalize(move);
        double sp = flySpeed * (in.run ? 5.0 : 1.0);
        pos_ += move * sp * double(dt);
        speed_ = float(glm::length(move) * sp);
        geo::fromEcef(pos_, lat_, lon_, h_);
        // Coarse ground check is enough while flying.
        double g;
        if (data.tryCoarseHeight(lat_, lon_, g)) ground_ = g;
        if (h_ < ground_ + 1.0) {
            h_ = ground_ + 1.0;
            pos_ = geo::toEcef(lat_, lon_, h_);
        }
        vUp_ = 0;
    } else {
        if (waitingForGround_) {
            double g;
            if (!data.tryMeshHeight(lat_, lon_, g)) {
                speed_ = 0;
                refreshFrame();
                updateCamera(data);
                return;
            }
            waitingForGround_ = false;
            ground_ = h_ = g;
            pos_ = geo::toEcef(lat_, lon_, h_);
        }
        float sp = in.run ? kRunSpeed : kWalkSpeed;
        glm::dvec3 step = wish * double(sp * dt);

        // Trunk collision: push the body circle out of every nearby trunk circle.
        trunkTimer_ -= dt;
        if (trunkTimer_ <= 0) {
            veg.nearbyTrunks(pos_, 6.0, trunks_);
            trunkTimer_ = 0.25f;
        }
        glm::dvec3 next = pos_ + step;
        for (auto& t : trunks_) {
            glm::dvec3 d = next - t.pos;
            d -= up_ * glm::dot(d, up_);
            double r = t.radius + kBodyRadius;
            double len = glm::length(d);
            if (len < r && len > 1e-6) next += d / len * (r - len);
        }

        double nlat, nlon, nh, g;
        geo::fromEcef(next, nlat, nlon, nh);
        if (!data.tryMeshHeight(nlat, nlon, g)) {
            // Ground under the next step is still streaming in: wait in place, no falling.
            speed_ = 0;
            refreshFrame();
            updateCamera(data);
            return;
        }
        lat_ = nlat;
        lon_ = nlon;
        ground_ = g;

        if (onGround_ && in.jump) {
            vUp_ = kJumpSpeed;
            onGround_ = false;
            jumped_ = true;
        }
        vUp_ -= kGravity * dt;
        h_ = (onGround_ ? ground_ : h_) + double(vUp_ * dt);
        if (h_ <= ground_) {
            h_ = ground_;
            vUp_ = 0;
            onGround_ = true;
        } else if (h_ > ground_ + 0.05) {
            onGround_ = false;
        }
        pos_ = geo::toEcef(lat_, lon_, h_);
        speed_ = glm::length(wish) > 0 ? sp : 0.0f;
        // Ground velocity in the body frame (the body may still be turning towards the view).
        if (speed_ > 0.0f) {
            glm::dvec3 v = wish * double(speed_);
            glm::dvec3 bf = north_ * double(std::cos(bodyYaw_)) + east_ * double(std::sin(bodyYaw_));
            glm::dvec3 br = east_ * double(std::cos(bodyYaw_)) - north_ * double(std::sin(bodyYaw_));
            localVel_ = glm::vec2(float(glm::dot(v, br)), float(glm::dot(v, bf)));
        }
        walkPhase_ += speed_ * dt * 1.6f;
    }
    refreshFrame();
    updateCamera(data);
}

void Player::previewCamera(float distance, float height, float horizontalOffset) {
    glm::dvec3 forward=north_*double(std::cos(yaw_))+east_*double(std::sin(yaw_));
    glm::dvec3 right=east_*double(std::cos(yaw_))-north_*double(std::sin(yaw_));
    glm::dvec3 look=forward*double(std::cos(pitch_))+up_*double(std::sin(pitch_));
    glm::dvec3 target=pos_+up_*double(height)-right*double(horizontalOffset);
    camPos_=target-look*double(distance);
    camFwd_=glm::vec3(look);
}

void Player::updateCamera(world::WorldData& data) {
    glm::dvec3 fwdH = north_ * double(std::cos(yaw_)) + east_ * double(std::sin(yaw_));
    glm::dvec3 look = fwdH * double(std::cos(pitch_)) + up_ * double(std::sin(pitch_));
    camFwd_ = glm::vec3(look);
    if (view == View::FirstPerson) {
        camPos_ = pos_ + up_ * double(eyeHeight_);
        return;
    }
    glm::dvec3 target = pos_ + up_ * double(eyeHeight_ - 0.2f);
    camPos_ = target - look * double(thirdPersonDistance);
    double clat, clon, ch;
    geo::fromEcef(camPos_, clat, clon, ch);
    double g;
    bool ok = flying ? data.tryCoarseHeight(clat, clon, g) : data.tryMeshHeight(clat, clon, g);
    if (ok && ch < g + 0.4) camPos_ += up_ * (g + 0.4 - ch);
    camFwd_ = glm::vec3(glm::normalize(target - camPos_));
}

}  // namespace game
