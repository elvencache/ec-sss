$input v_texcoord0

/*
* Copyright 2021 elven cache. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

#include "../common/common.sh"
#include "parameters.sh"

SAMPLER2D(s_depth, 0);

// from assao sample, cs_assao_prepare_depths.sc
vec3 NDCToViewspace( vec2 pos, float viewspaceDepth )
{
	vec3 ret;

	ret.xy = (u_ndcToViewMul * pos.xy + u_ndcToViewAdd) * viewspaceDepth;

	ret.z = viewspaceDepth;

	return ret;
}

#define LIGHT_STEPS		32

void main()
{
	vec2 texCoord = v_texcoord0;
	float linearDepth = texture2D(s_depth, texCoord).x;
	vec3 viewSpacePosition = NDCToViewspace(texCoord, linearDepth);

	vec3 toLight = u_lightPosition - viewSpacePosition;
	vec3 light = normalize(toLight);

	vec3 debugColor = light * 0.5 + 0.5;

	// add drop shadow below light, y is up
	mat4 worldToView = mat4(
		u_worldToView0,
		u_worldToView1,
		u_worldToView2,
		u_worldToView3
	);
	vec3 worldUpInViewSpace = instMul(worldToView, vec4(0.0, 1.0, 0.0, 0.0)).xyz;
	float shadow = 1.0 - smoothstep(0.95, 1.0, dot(light, worldUpInViewSpace));
	debugColor *= shadow;

	gl_FragColor = vec4(debugColor, 1.0);
}
