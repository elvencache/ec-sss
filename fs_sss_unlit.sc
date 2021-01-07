$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3

#include "../common/common.sh"
#include "parameters.sh"
#include "normal_encoding.sh"

void main()
{
	vec3 albedo = vec3_splat(1.0);

	// get vertex normal
	vec3 normal = normalize(v_normal);
	float roughness = 1.0;

	// Calculate velocity as delta position from previous frame to this
	vec2 previousNDC = v_texcoord1.xy * (1.0/v_texcoord1.w);
	previousNDC.y *= -1.0;
	previousNDC = previousNDC * 0.5 + 0.5;
	vec2 velocity = gl_FragCoord.xy*u_viewTexel.xy - previousNDC;
	
	vec3 bufferNormal = NormalEncode(normal);

	// write data to alpha channel of color buffer to signify different handling
	// while lighting/shading these pixels in the gbuffer combine pass
	gl_FragData[0] = vec4(toGamma(albedo), 0.0);
	gl_FragData[1] = vec4(bufferNormal, roughness);
	gl_FragData[2] = vec4(velocity, 0.0, 0.0);
}
