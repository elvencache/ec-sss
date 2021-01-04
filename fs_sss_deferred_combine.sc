$input v_texcoord0

/*
* Copyright 2021 elven cache. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

#include "../common/common.sh"
#include "parameters.sh"
#include "normal_encoding.sh"

SAMPLER2D(s_color, 0);
SAMPLER2D(s_normal, 1);
SAMPLER2D(s_depth, 2);
SAMPLER2D(s_shadows, 3);

float ShadertoyNoise (vec2 uv) {
	return fract(sin(dot(uv.xy, vec2(12.9898,78.233))) * 43758.5453123);
}

int ModHelper (float a, float b)
{
	return int( a - (b*floor(a/b)));
}

// from assao sample, cs_assao_prepare_depths.sc
vec3 NDCToViewspace( vec2 pos, float viewspaceDepth )
{
	vec3 ret;

	ret.xy = (u_ndcToViewMul * pos.xy + u_ndcToViewAdd) * viewspaceDepth;

	ret.z = viewspaceDepth;

	return ret;
}

void main()
{
	vec2 texCoord = v_texcoord0;

	vec3 color = texture2D(s_color, texCoord).xyz;
	color = toLinear(color);

	vec4 normalRoughness = texture2D(s_normal, texCoord);
	vec3 normal = NormalDecode(normalRoughness.xyz);
	float roughness = normalRoughness.w;

	// transform normal into view space
	mat4 worldToViewPrev = mat4(
		u_worldToViewPrev0,
		u_worldToViewPrev1,
		u_worldToViewPrev2,
		u_worldToViewPrev3
	);
	vec3 vsNormal = instMul(worldToViewPrev, vec4(normal, 0.0)).xyz;


	// read depth and recreate position
	float linearDepth = texture2D(s_depth, texCoord);
	vec3 viewSpacePosition = NDCToViewspace(texCoord, linearDepth);

	float shadow = texture2D(s_shadows, texCoord).x;

	// need to get a valid view vector for any microfacet stuff :(
	float gloss = 1.0-roughness;
	float specPower = 62.0 * gloss + 2.0;

	vec3 light = normalize(u_lightPosition - viewSpacePosition);
	//vec3 light = normalize(vec3(-0.2, 1.0, -0.2));
	float NdotL = saturate(dot(vsNormal, light));
	float ambient = 0.1;
	float diffuse = NdotL;
	float specular = 5.0 * pow(NdotL, specPower);

	float lightAmount = mix(diffuse, specular, 0.04) * shadow + ambient;

	color = (color * lightAmount);
	color = color / (color + vec3_splat(1.0));
	color = toGamma(color);

	gl_FragColor = vec4(color * lightAmount, 1.0);
}
