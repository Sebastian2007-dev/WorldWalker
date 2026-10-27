#include "render/Character.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace render {
namespace {

static_assert(sizeof(SkinVertex) == 40);

struct SkinPush {
    glm::mat4 model;
    VkDeviceAddress frame, vertices, joints;
    uint32_t albedo, normal, alphaTest;
    uint32_t material;
    glm::vec3 tint;
    float normalScale;
};
static_assert(sizeof(SkinPush) <= gfx::kPushConstantSize);

template <typename T>
bool readRaw(std::ifstream& f, T* dst, size_t count) {
    f.read(reinterpret_cast<char*>(dst), std::streamsize(sizeof(T) * count));
    return bool(f);
}

float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Rotation taking unit vector a onto unit vector b along the shortest arc.
glm::quat shortestArc(glm::vec3 a, glm::vec3 b) {
    float d = glm::dot(a, b);
    if (d > 0.99999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), a);
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(glm::vec3(0, 1, 0), a);
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

glm::mat4 boneMatrix(const glm::quat& r, const glm::vec3& p) {
    glm::mat4 m = glm::mat4_cast(r);
    m[3] = glm::vec4(p, 1.0f);
    return m;
}

// The child whose direction defines a bone's orientation for retargeting.
int mainChild(const std::vector<SkeletonBone>& bones, int b) {
    static const std::unordered_map<std::string, std::string> preferred = {
        {"Spine", "Spine1"}, {"Spine1", "Spine2"}, {"Spine2", "Neck"},
        {"LeftHand", "LeftHandMiddle1"}, {"RightHand", "RightHandMiddle1"}};
    auto it = preferred.find(bones[size_t(b)].name);
    int first = -1;
    for (size_t c = 0; c < bones.size(); ++c) {
        if (bones[c].parent != b) continue;
        if (it != preferred.end() && bones[c].name == it->second) return int(c);
        if (first < 0) first = int(c);
    }
    return first;
}

}  // namespace

bool Character::init(gfx::Device& dev, gfx::Bindless& bindless, const std::filesystem::path& animDir,
                     VkFormat colorFormat, VkFormat depthFormat) {
    dev_ = &dev;
    bindless_ = &bindless;
    for (auto& j : joints_)
        j = dev.createBuffer(kMaxBones * sizeof(glm::mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, true);

    VkPipelineLayout layout = bindless.pipelineLayout();
    gfx::GraphicsPipelineDesc d;
    d.colorFormats = {colorFormat};
    d.depthFormat = depthFormat;
    d.vs = "skinned.vert";
    d.fs = "skinned.frag";
    pipe_ = gfx::createGraphicsPipeline(dev, layout, d);
    d.cull = VK_CULL_MODE_NONE;  // hair cards are seen from both sides
    hairPipe_ = gfx::createGraphicsPipeline(dev, layout, d);
    gfx::GraphicsPipelineDesc sd;
    sd.depthFormat = depthFormat;
    sd.cull = VK_CULL_MODE_NONE;
    sd.vs = "skinned.vert";
    sd.fs = "skinned_shadow.frag";
    shadowPipe_ = gfx::createGraphicsPipeline(dev, layout, sd);

    return loadClips(animDir);
}

void Character::shutdown() {
    if (!dev_) return;
    for (auto& t : textures_) dev_->destroyNow(t);
    for (gfx::Buffer* b : {&vertices_, &indices_}) dev_->destroyNow(*b);
    for (auto& b : retiredMeshes_) dev_->destroyNow(b);
    retiredMeshes_.clear();
    for (auto& j : joints_) dev_->destroyNow(j);
    for (VkPipeline p : {pipe_, hairPipe_, shadowPipe_})
        if (p) vkDestroyPipeline(dev_->device(), p, nullptr);
}

uint32_t Character::loadTexture(const std::filesystem::path& file, bool srgb) {
    std::string key = file.string() + (srgb ? "|s" : "|l");
    if (auto it = textureCache_.find(key); it != textureCache_.end()) return it->second;
    int w, h, n;
    stbi_uc* px = file.empty() ? nullptr : stbi_load(file.string().c_str(), &w, &h, &n, 4);
    const uint32_t fallback = srgb ? 0xFF808080u : 0xFFFF8080u;
    if (!px && !file.empty()) std::fprintf(stderr, "missing character texture %s\n", file.string().c_str());
    gfx::Image img = dev_->createTexture2D(px ? (const void*)px : (const void*)&fallback, px ? uint32_t(w) : 1,
                                           px ? uint32_t(h) : 1, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                                           px != nullptr);
    if (px) stbi_image_free(px);
    textures_.push_back(img);
    uint32_t index = bindless_->add(img, bindless_->linearRepeat());
    textureCache_[key] = index;
    return index;
}

bool Character::loadBakedMesh(const std::filesystem::path& dir) {
    std::ifstream jf(dir / "character.json");
    if (!jf) {
        std::fprintf(stderr, "No baked character in %s (run tools/blender/bake_character.py)\n", dir.string().c_str());
        return false;
    }
    nlohmann::json meta = nlohmann::json::parse(jf);
    // WWC1, see bake_character.py
    std::ifstream mf(dir / meta["mesh"].get<std::string>(), std::ios::binary);
    char magic[4];
    uint32_t hdr[4];
    if (!readRaw(mf, magic, 4) || std::memcmp(magic, "WWC1", 4) != 0 || !readRaw(mf, hdr, 4)) return false;
    uint32_t boneCount = hdr[0], vertexCount = hdr[1], indexCount = hdr[2], submeshCount = hdr[3];
    if (boneCount > kMaxBones) return false;
    struct Submesh { uint32_t firstIndex, indexCount, material; };
    std::vector<Submesh> subs(submeshCount);
    readRaw(mf, subs.data(), submeshCount);
    std::vector<SkeletonBone> bones(boneCount);
    for (auto& b : bones) {
        char name[32];
        int32_t parent;
        glm::mat4 inv;
        readRaw(mf, name, 32);
        readRaw(mf, &parent, 1);
        readRaw(mf, &inv[0][0], 16);
        name[31] = 0;
        glm::mat4 bind = glm::inverse(inv);
        b.name = name;
        b.parent = parent;
        b.rot = glm::normalize(glm::quat_cast(glm::mat3(bind)));
        b.pos = glm::vec3(bind[3]);
    }
    std::vector<SkinVertex> verts(vertexCount);
    std::vector<uint32_t> idx(indexCount);
    if (!readRaw(mf, verts.data(), vertexCount) || !readRaw(mf, idx.data(), indexCount)) return false;
    std::vector<MeshPart> parts;
    std::vector<std::pair<uint32_t, uint32_t>> mats;
    std::vector<bool> alpha;
    for (auto& m : meta["materials"]) {
        mats.push_back({loadTexture(dir / m["albedo"].get<std::string>(), true),
                        loadTexture(dir / m["normal"].get<std::string>(), false)});
        alpha.push_back(m["alphaTest"].get<bool>());
    }
    for (const Submesh& s : subs)
        parts.push_back({s.firstIndex, s.indexCount, mats[s.material].first, mats[s.material].second, bool(alpha[s.material])});
    setSkeleton(bones);
    setMesh(verts, idx, parts);
    std::printf("Character: %u bones, %u vertices\n", boneCount, vertexCount);
    return true;
}

bool Character::loadClips(const std::filesystem::path& dir) {
    std::ifstream jf(dir / "character.json");
    if (!jf) return false;
    nlohmann::json meta = nlohmann::json::parse(jf);
    for (auto& c : meta["clips"]) {
        std::ifstream af(dir / c["file"].get<std::string>(), std::ios::binary);
        char magic[4];
        uint32_t ah[2];
        float fps;
        if (!readRaw(af, magic, 4) || std::memcmp(magic, "WWA2", 4) != 0 || !readRaw(af, ah, 2) || !readRaw(af, &fps, 1) ||
            ah[1] < 2)
            continue;
        uint32_t nb = ah[0];
        Clip clip;
        clip.name = c["name"].get<std::string>();
        clip.frames = ah[1];
        clip.fps = fps;
        clip.duration = float(clip.frames - 1) / fps;
        for (uint32_t b = 0; b < nb; ++b) {
            char name[32];
            int32_t parent;
            float v[7];
            readRaw(af, name, 32);
            readRaw(af, &parent, 1);
            readRaw(af, v, 7);
            name[31] = 0;
            clip.boneNames.push_back(name);
            clip.boneParent.push_back(parent);
            clip.restPos.emplace_back(v[0], v[1], v[2]);
            clip.restRot.push_back(glm::normalize(glm::quat(v[6], v[3], v[4], v[5])));
        }
        bool ok = true;
        for (uint32_t f = 0; f < clip.frames && ok; ++f) {
            float h[3];
            ok = readRaw(af, h, 3);
            clip.hips.emplace_back(h[0], h[1], h[2]);
            for (uint32_t b = 0; b < nb && ok; ++b) {
                float q[4];
                ok = readRaw(af, q, 4);
                glm::quat pose = glm::normalize(glm::quat(q[3], q[0], q[1], q[2]));
                clip.delta.push_back(glm::normalize(pose * glm::conjugate(clip.restRot[b])));
            }
        }
        if (!ok) continue;
        // Mixamo locomotion moves the hips forward; that travel is removed at playback and turned into a speed.
        clip.rootMotion = glm::vec2(clip.hips.back() - clip.hips.front());
        clip.speed = glm::length(clip.rootMotion) / clip.duration;
        // Turn clips rotate the hips about the vertical; integrate frame to frame so turns beyond 180 degrees count.
        for (uint32_t fr = 1; fr < clip.frames; ++fr) {
            glm::quat d = clip.delta[size_t(fr) * nb] * glm::conjugate(clip.delta[size_t(fr - 1) * nb]);
            if (d.w < 0.0f) d = -d;
            clip.rootYaw += 2.0f * std::atan2(d.z, d.w);
        }
        if (std::abs(clip.rootYaw) < 0.3f) clip.rootYaw = 0.0f;
        clips_.push_back(std::move(clip));
    }
    layers_.resize(clips_.size());
    for (size_t i = 0; i < layers_.size(); ++i) layers_[i].clip = int(i);
    idle_ = findClip("idle");
    walk_ = findClip("walking");
    run_ = findClip("running");
    back_ = findClip("walking_backwards");
    backRun_ = findClip("running_backward");
    strafeL_ = findClip("left_strafe_walking");
    strafeR_ = findClip("right_strafe_walking");
    strafeRunL_ = findClip("left_strafe");
    strafeRunR_ = findClip("right_strafe");
    jump_ = findClip("jump");
    fall_ = findClip("falling_idle");
    land_ = findClip("hard_landing");
    turnL_ = findClip("left_turn");
    turnR_ = findClip("right_turn");
    kneel_ = findClip("kneeling_idle");
    // Clips exported "in place" have no hip travel to measure their speed from: borrow the forward counterpart's.
    auto borrowSpeed = [&](int c, int from) {
        if (c >= 0 && from >= 0 && clips_[size_t(c)].speed < 0.1f) clips_[size_t(c)].speed = clips_[size_t(from)].speed;
    };
    borrowSpeed(backRun_, run_);
    borrowSpeed(back_, walk_);
    if (kneel_ >= 0) layers_[size_t(kneel_)].fadeRate = 3.0f;  // kneeling down takes a moment
    for (int c : {jump_, land_})
        if (c >= 0) layers_[size_t(c)].loop = false;
    if (idle_ >= 0) layers_[size_t(idle_)].weight = 1.0f;
    std::printf("Animation: %zu clips\n", clips_.size());
    return !clips_.empty();
}

void Character::setSkeleton(const std::vector<SkeletonBone>& bones) {
    bones_.assign(bones.begin(), bones.begin() + std::min<size_t>(bones.size(), kMaxBones));
    inverseBind_.resize(bones_.size());
    for (size_t b = 0; b < bones_.size(); ++b) inverseBind_[b] = glm::inverse(boneMatrix(bones_[b].rot, bones_[b].pos));
    palette_.assign(bones_.size(), glm::mat4(1.0f));
    retarget();
    samplePose();
}

void Character::retarget() {
    size_t nb = bones_.size();
    for (Clip& c : clips_) {
        c.map.assign(nb, -1);
        for (size_t b = 0; b < nb; ++b)
            for (size_t k = 0; k < c.boneNames.size(); ++k)
                if (c.boneNames[k] == bones_[b].name) c.map[b] = int(k);
    }
    align_.assign(nb, glm::quat(1, 0, 0, 0));
    hipScale_ = 1.0f;
    if (clips_.empty() || nb == 0) return;
    // All clips share the source skeleton they were exported with; the first one describes its rest pose.
    const Clip& src = clips_[0];
    for (size_t b = 0; b < nb; ++b) {
        int parent = bones_[b].parent;
        glm::quat inherited = parent >= 0 ? align_[size_t(parent)] : glm::quat(1, 0, 0, 0);
        int child = mainChild(bones_, int(b));
        align_[b] = inherited;
        if (parent < 0 || child < 0) continue;  // the hips keep their orientation; leaves follow their parent
        int sb = src.map[b], sc = src.map[size_t(child)];
        if (sb < 0 || sc < 0) continue;
        glm::vec3 td = bones_[size_t(child)].pos - bones_[b].pos, sd = src.restPos[size_t(sc)] - src.restPos[size_t(sb)];
        if (glm::length(td) < 1e-4f || glm::length(sd) < 1e-4f) continue;
        align_[b] = shortestArc(glm::normalize(td), glm::normalize(sd));
    }
    int hipsSrc = src.map[0];
    if (hipsSrc >= 0 && src.restPos[size_t(hipsSrc)].z > 0.1f)
        hipScale_ = bones_[0].pos.z / src.restPos[size_t(hipsSrc)].z;
}

void Character::setMesh(const std::vector<SkinVertex>& vertices, const std::vector<uint32_t>& indices,
                        const std::vector<MeshPart>& parts) {
    // Frames in flight may still read the old buffers: destroy them once the GPU is done.
    // Edits happen before beginFrame. Keep old buffers alive through its upload flush and garbage collection.
    if (vertices_) retiredMeshes_.push_back(vertices_);
    if (indices_) retiredMeshes_.push_back(indices_);
    vertices_ = {};
    indices_ = {};
    vertices_ = dev_->createBuffer(vertices.size() * sizeof(SkinVertex),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY);
    dev_->uploadBuffer(vertices_, 0, vertices.data(), vertices_.size);
    indices_ = dev_->createBuffer(indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY, false, false);
    dev_->uploadBuffer(indices_, 0, indices.data(), indices_.size);
    parts_ = parts;
}

int Character::findClip(const char* name) const {
    for (size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].name == name) return int(i);
    return -1;
}

void Character::setTarget(int clip, float w) {
    if (clip >= 0) layers_[size_t(clip)].target += w;
}

void Character::update(float dt, const AnimInput& in) {
    if (!loaded() || clips_.empty()) return;
    if (!animated_) { samplePose(); return; }
    for (auto& l : layers_) l.target = 0.0f;

    // ---- airborne: jump take-off, then falling; long falls end in a landing
    bool airborne = in.flying || !in.grounded;
    if (in.jumped && jump_ >= 0) {
        Layer& j = layers_[size_t(jump_)];
        j.time = clips_[size_t(jump_)].duration * 0.28f;  // skip the crouch: physics takes off immediately
        j.weight = std::max(j.weight, 0.5f);
    }
    if (!airborne && airTime_ > 1.0f && land_ >= 0) {
        landTimer_ = 0.6f;
        layers_[size_t(land_)].time = clips_[size_t(land_)].duration * 0.15f;
    }
    airTime_ = airborne ? airTime_ + dt : 0.0f;
    landTimer_ = std::max(landTimer_ - dt, 0.0f);

    if (airborne) {
        bool inJump = jump_ >= 0 && layers_[size_t(jump_)].time < clips_[size_t(jump_)].duration * 0.55f && !in.flying &&
                      airTime_ < 1.0f;
        setTarget(inJump ? jump_ : fall_, 1.0f);
    } else if (landTimer_ > 0.0f) {
        setTarget(land_, 1.0f);
    } else if (in.kneeling && kneel_ >= 0) {
        setTarget(kneel_, 1.0f);
    } else if (std::abs(in.turnRate) > 0.4f && glm::length(in.velocity) < 0.1f) {
        // Turning in place: the turn clip runs at the speed the body actually turns.
        int c = in.turnRate > 0.0f ? turnR_ : turnL_;
        if (c >= 0 && clips_[size_t(c)].rootYaw != 0.0f) {
            setTarget(c, 1.0f);
            Layer& l = layers_[size_t(c)];
            float clipRate = std::abs(clips_[size_t(c)].rootYaw) / clips_[size_t(c)].duration;
            l.time = std::fmod(l.time + dt * (std::abs(in.turnRate) / clipRate - 1.0f) + clips_[size_t(c)].duration,
                               clips_[size_t(c)].duration);  // the generic clock below adds the other dt
        } else {
            setTarget(idle_, 1.0f);
        }
    } else {
        // ---- ground locomotion: direction picks the clips, speed blends walk -> run and sets the playback rate
        float s = glm::length(in.velocity);
        glm::vec2 dir = s > 0.05f ? in.velocity / s : glm::vec2(0, 1);
        float fw = std::max(dir.y, 0.0f), bw = std::max(-dir.y, 0.0f), lw = std::max(-dir.x, 0.0f), rw = std::max(dir.x, 0.0f);
        float sum = std::max(fw + bw + lw + rw, 1e-3f);
        fw /= sum, bw /= sum, lw /= sum, rw /= sum;
        float moving = smoothstep(0.1f, 0.6f, s), run = smoothstep(2.2f, 4.0f, s);
        setTarget(idle_, 1.0f - moving);
        setTarget(walk_, moving * fw * (1.0f - run));
        setTarget(run_, moving * fw * run);
        setTarget(back_, moving * bw * (backRun_ >= 0 ? 1.0f - run : 1.0f));
        setTarget(backRun_, moving * bw * run);
        setTarget(strafeL_, moving * lw * (1.0f - run));
        setTarget(strafeR_, moving * rw * (1.0f - run));
        setTarget(strafeRunL_, moving * lw * run);
        setTarget(strafeRunR_, moving * rw * run);

        // One shared phase keeps the feet of blended locomotion clips in step.
        float rate = 0.0f, wsum = 0.0f;
        for (int c : {walk_, run_, back_, backRun_, strafeL_, strafeR_, strafeRunL_, strafeRunR_}) {
            if (c < 0 || layers_[size_t(c)].target <= 0.0f) continue;
            const Clip& clip = clips_[size_t(c)];
            float clipSpeed = clip.speed * hipScale_;  // longer legs, longer strides
            float playback = clipSpeed > 0.1f ? std::clamp(s / clipSpeed, 0.5f, 1.8f) : 1.0f;
            rate += layers_[size_t(c)].target * playback / clip.duration;
            wsum += layers_[size_t(c)].target;
        }
        if (wsum > 0.0f) locoPhase_ = std::fmod(locoPhase_ + dt * rate / wsum, 1.0f);
        for (int c : {walk_, run_, back_, backRun_, strafeL_, strafeR_, strafeRunL_, strafeRunR_})
            if (c >= 0) layers_[size_t(c)].time = locoPhase_ * clips_[size_t(c)].duration;
    }

    // Cross-fade towards the targets; non-locomotion clips run on their own clocks.
    for (size_t i = 0; i < layers_.size(); ++i) {
        Layer& l = layers_[i];
        // Fade with the slower rate of the clip coming in and the one going out (kneeling down and getting up).
        float rate = l.fadeRate;
        if (kneel_ >= 0 && layers_[size_t(kneel_)].weight > 0.05f) rate = std::min(rate, layers_[size_t(kneel_)].fadeRate);
        l.weight += (l.target - l.weight) * (1.0f - std::exp(-dt * rate));
        bool loco = int(i) == walk_ || int(i) == run_ || int(i) == back_ || int(i) == backRun_ || int(i) == strafeL_ || int(i) == strafeR_ ||
                    int(i) == strafeRunL_ || int(i) == strafeRunR_;
        if (!loco && l.weight > 0.001f) {
            float dur = clips_[i].duration;
            l.time += dt;
            l.time = l.loop ? std::fmod(l.time, dur) : std::min(l.time, dur);
        }
    }
    samplePose();
}

void Character::samplePose() {
    size_t nb = bones_.size();
    if (nb == 0) return;
    // Blend the clips' model space rotation deltas and hip offsets.
    std::vector<glm::vec4> r(nb, glm::vec4(0.0f));
    std::vector<bool> animated(nb, false);
    glm::vec3 hipOffset(0.0f);
    float total = 0.0f;
    for (size_t li = 0; li < layers_.size(); ++li) {
        const Layer& l = layers_[li];
        if (l.weight < 0.001f) continue;
        const Clip& c = clips_[li];
        size_t cb = c.boneNames.size();
        float phase = std::clamp(l.time / c.duration, 0.0f, 1.0f);
        float f = phase * float(c.frames - 1);
        uint32_t f0 = std::min(uint32_t(f), c.frames - 2), f1 = f0 + 1;
        float a = f - float(f0);
        int hipsBone = c.map.empty() ? -1 : c.map[0];
        if (hipsBone >= 0) {
            glm::vec3 h = glm::mix(c.hips[f0], c.hips[f1], a) - glm::vec3(c.rootMotion * phase, 0.0f);
            hipOffset += (h - c.restPos[size_t(hipsBone)]) * l.weight;
        }
        for (size_t b = 0; b < nb; ++b) {
            int k = c.map[b];
            if (k < 0) continue;
            glm::quat qb = glm::slerp(c.delta[size_t(f0) * cb + size_t(k)], c.delta[size_t(f1) * cb + size_t(k)], a);
            if (b == 0 && c.rootYaw != 0.0f)  // the body itself does the turning
                qb = glm::angleAxis(-c.rootYaw * phase, glm::vec3(0, 0, 1)) * qb;
            glm::vec4 qv(qb.x, qb.y, qb.z, qb.w);
            if (glm::dot(qv, r[b]) < 0.0f) qv = -qv;  // blend in one hemisphere
            r[b] += qv * l.weight;
            animated[b] = true;
        }
        total += l.weight;
    }
    if (total <= 0.0f || !animated_) {  // bind pose
        if (!animated_) std::fill(palette_.begin(), palette_.end(), glm::mat4(1.0f));
        return;
    }
    hipOffset /= total;

    // Retargeted pose: bone rotation = delta * align * bind; children hang off their parent at bind distance.
    std::vector<glm::quat> rot(nb);   // delta * align (model space)
    std::vector<glm::vec3> pos(nb);
    for (size_t b = 0; b < nb; ++b) {
        int p = bones_[b].parent;
        glm::quat delta;
        if (animated[b]) {
            glm::vec4 qv = glm::normalize(r[b]);
            delta = glm::quat(qv.w, qv.x, qv.y, qv.z);
        } else {
            delta = p >= 0 ? rot[size_t(p)] * glm::conjugate(align_[size_t(p)]) : glm::quat(1, 0, 0, 0);
        }
        rot[b] = delta * align_[b];
        if (p < 0) pos[b] = bones_[b].pos + hipOffset * hipScale_;
        else pos[b] = pos[size_t(p)] + rot[size_t(p)] * (bones_[b].pos - bones_[size_t(p)].pos);
        palette_[b] = boneMatrix(rot[b] * bones_[b].rot, pos[b]) * inverseBind_[b];
    }
}

void Character::prepare(uint32_t frameIndex) {
    for (auto& b : retiredMeshes_) dev_->destroyLater(b);
    retiredMeshes_.clear();
    if (!loaded()) return;
    frameIndex_ = frameIndex;
    std::memcpy(joints_[frameIndex].mapped, palette_.data(), palette_.size() * sizeof(glm::mat4));
}

void Character::draw(VkCommandBuffer cmd, VkDeviceAddress frameData, const glm::mat4& model, bool shadow) {
    if (!loaded()) return;
    VkPipelineLayout layout = bindless_->pipelineLayout();
    VkDescriptorSet set = bindless_->set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const MeshPart& m : parts_) {
        if (m.indexCount == 0) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? shadowPipe_ : (m.alphaTest ? hairPipe_ : pipe_));
        SkinPush p{model,    frameData, vertices_.address, joints_[frameIndex_].address, m.albedo, m.normal,
                   m.alphaTest ? 1u : 0u, uint32_t(m.shading) | (m.recolor ? 0x100u : 0u), m.tint, m.normalScale};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, m.firstIndex, 0, 0);
    }
}

}  // namespace render
