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
} pc;
