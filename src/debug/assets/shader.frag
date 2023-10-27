// Copyright (c) 2017-2020 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma fragment

layout(binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;
layout (location = 0) out vec4 outFragColor;

void main()
{
	vec4 sampledColor = texture(texSampler, texCoord);
	vec4 dColor = vec4(sampledColor.rgb * sampledColor.a + color * (1 - sampledColor.a), 1.0);
	outFragColor = dColor;
}
