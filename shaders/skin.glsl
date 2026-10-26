// Skinned character: vertex format and push constants. Mirrors src/render/Character.cpp.
struct SkinVertex { vec3 pos; vec3 normal; vec2 uv; uint joints; uint weights; };  // joints/weights: 4 x u8
layout(buffer_reference, scalar) readonly buffer SkinVertexRef { SkinVertex d[]; };
layout(buffer_reference, scalar) readonly buffer JointRef { mat4 m[]; };

layout(push_constant, scalar) uniform Push {
    mat4 model;        // model space -> camera relative world
    FrameRef frame;
    SkinVertexRef vertices;
    JointRef joints;   // skinning palette: pose * inverse bind
    uint albedo;
    uint normal;
    uint alphaTest;
    uint material;     // kSkin* shading model, optionally | kSkinRecolor
    vec3 tint;         // linear albedo multiplier (with kSkinRecolor: multiplies the texture's luminance instead)
    float normalScale; // strength of the normal map's relief
} pc;

const uint kSkinCloth = 0u, kSkinBody = 1u, kSkinHair = 2u, kSkinEyes = 3u, kSkinUnderwear = 4u;
const uint kSkinRecolor = 0x100u;
