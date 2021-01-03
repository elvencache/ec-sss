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
SAMPLER2D(s_shadows, 2);

float ShadertoyNoise (vec2 uv) {
	return fract(sin(dot(uv.xy, vec2(12.9898,78.233))) * 43758.5453123);
}

int ModHelper (float a, float b)
{
	return int( a - (b*floor(a/b)));
}

void main()
{
	vec2 texCoord = v_texcoord0;

	vec3 color = texture2D(s_color, texCoord).xyz;
	color = toLinear(color);

	vec4 normalRoughness = texture2D(s_normal, texCoord);
	vec3 normal = NormalDecode(normalRoughness.xyz);
	float roughness = normalRoughness.w;

	float shadow = texture2D(s_shadows, texCoord).x;

	// need to get a valid view vector for any microfacet stuff :(
	float gloss = 1.0-roughness;
	float specPower = 62.0 * gloss + 2.0;

	vec3 light = normalize(vec3(-0.2, 1.0, -0.2));
	float NdotL = saturate(dot(normal, light));
	float ambient = 0.1;
	float diffuse = NdotL;
	float specular = 5.0 * pow(NdotL, specPower);

	float lightAmount = mix(diffuse, specular, 0.04) * shadow + ambient;

	color = (color * lightAmount);
	color = color / (color + vec3_splat(1.0));

	color = toGamma(color);

	gl_FragColor = vec4(color * lightAmount, 1.0);
}
