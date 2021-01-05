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

#define SHADOW_RADIUS	0.25
#define SHADOW_STEPS	16

void main()
{
	vec2 texCoord = v_texcoord0;
	float linearDepth = texture2D(s_depth, texCoord).x;
	vec3 viewSpacePosition = NDCToViewspace(texCoord, linearDepth);

	vec3 toLight = u_lightPosition - viewSpacePosition;
	vec3 light = normalize(toLight);

	vec3 lightStep = normalize(toLight) * (SHADOW_RADIUS / float(SHADOW_STEPS));

	float occluded = 0.0;
	vec3 samplePosition = viewSpacePosition;
	mat4 viewToProj = mat4(
		u_viewToProj0,
		u_viewToProj1,
		u_viewToProj2,
		u_viewToProj3
	);

	float firstHit = float(SHADOW_STEPS);
	float lastHit = -1.0;
	for (int i = 0; i < SHADOW_STEPS; ++i)
	{
		samplePosition += lightStep;
		vec3 psSamplePosition = instMul(viewToProj, vec4(samplePosition, 1.0)).xyw;
		psSamplePosition.xy *= (1.0/psSamplePosition.z);
		vec2 sampleCoord = psSamplePosition.xy * 0.5 + 0.5;
		sampleCoord.y = 1.0 - sampleCoord.y;
		float sampleDepth = texture2D(s_depth, sampleCoord).x;

		float delta = (samplePosition.z - sampleDepth);
		if (1e-5 < delta)
		{
			firstHit = min(firstHit, float(i));
			lastHit = max(lastHit, float(i));
			occluded += saturate(SHADOW_RADIUS - delta);//1.0;
		}
	}
	occluded *= (SHADOW_STEPS - firstHit) / SHADOW_STEPS;

	gl_FragColor = vec4_splat(1.0-occluded);
}
