$input v_texcoord0

/*
* Copyright 2021 elven cache. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

#include "../common/common.sh"
#include "parameters.sh"

SAMPLER2D(s_depth, 0);

#define DEPTH_EPSILON	1e-4

// from assao sample, cs_assao_prepare_depths.sc
vec3 NDCToViewspace( vec2 pos, float viewspaceDepth )
{
	vec3 ret;

	ret.xy = (u_ndcToViewMul * pos.xy + u_ndcToViewAdd) * viewspaceDepth;

	ret.z = viewspaceDepth;

	return ret;
}

float ShadertoyNoise (vec2 uv) {
	return fract(sin(dot(uv.xy, vec2(12.9898,78.233))) * 43758.5453123);
}

void main()
{
	vec2 texCoord = v_texcoord0;
	float linearDepth = texture2D(s_depth, texCoord).x;
	vec3 viewSpacePosition = NDCToViewspace(texCoord, linearDepth);

	vec3 toLight = u_lightPosition - viewSpacePosition;
	vec3 lightStep = normalize(toLight) * (u_shadowRadius / u_shadowSteps);

	vec3 samplePosition = viewSpacePosition;
	float random = ShadertoyNoise(gl_FragCoord.xy + vec2(314.0, 159.0)*u_frameIdx);
	float initialOffset = (0.0 < u_useNoiseOffset) ? (0.5+random) : 1.0;
	samplePosition += initialOffset * lightStep;

	mat4 viewToProj = mat4(
		u_viewToProj0,
		u_viewToProj1,
		u_viewToProj2,
		u_viewToProj3
	);

	float occluded = 0.0;
	float firstHit = u_shadowSteps;
	for (int i = 0; i < int(u_shadowSteps); ++i, samplePosition += lightStep)
	{
		vec3 psSamplePosition = instMul(viewToProj, vec4(samplePosition, 1.0)).xyw;
		psSamplePosition.xy *= (1.0/psSamplePosition.z);

		vec2 sampleCoord = psSamplePosition.xy * 0.5 + 0.5;
		sampleCoord.y = 1.0 - sampleCoord.y;
		float sampleDepth = texture2D(s_depth, sampleCoord).x;

		float delta = (samplePosition.z - sampleDepth);
		if (DEPTH_EPSILON < delta && delta < u_shadowRadius)
		{
			firstHit = min(firstHit, float(i));
			occluded += saturate(u_shadowRadius - delta);
		}
	}

	float shadow;
	if (0.0 < u_useSoftContactShadows)
	{
		shadow = occluded * (1.0 - (firstHit / u_shadowSteps));
		shadow = 1.0 - saturate(occluded);
		shadow = shadow*shadow;
	}
	else
	{
		shadow = 0.0 < occluded ? 0.0 : 1.0;
	}

	gl_FragColor = vec4_splat(shadow);
}
