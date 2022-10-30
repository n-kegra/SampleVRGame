// Copyright (c) 2017-2020 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma vertex

layout (std140, push_constant) uniform buf
{
    mat4 mvp;
	vec3 baseColor;
} ubuf;

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 texCoord;

layout (location = 0) out vec2 outTexCoord;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec3 outColor;
out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
    gl_Position = ubuf.mvp * vec4(position, 1);
    outTexCoord = texCoord;
    outNormal = normal;
    outColor = ubuf.baseColor;
}
