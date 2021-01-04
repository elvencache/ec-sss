$input v_texcoord0

/*
* Copyright 2021 elven cache. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

#include "../common/common.sh"
#include "parameters.sh"

SAMPLER2D(s_depth, 0);

void main()
{
	vec2 texCoord = v_texcoord0;
	float depth = texture2D(s_depth, texCoord).x;

	gl_FragColor = vec4_splat(1.0);
}
