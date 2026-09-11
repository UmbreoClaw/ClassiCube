/* Second composite pass: blends the translucent water layer over everything drawn so far
   (opaque world, entities, particles), using premultiplied alpha. Writes the water
   surface depth, like the rasteriser's translucent pass does. */
layout(binding = 6) uniform sampler2D albedoTex;  /* w = distance to the water surface along the pixel ray */
layout(binding = 7) uniform sampler2D waterTex;   /* rgb = premultiplied colour, a = alpha */

out vec4 fragColour;

void main() {
	ivec2 px = ivec2(vec2(gl_FragCoord.xy) * vec2(screen.xy) / vec2(window.xy));
	px = clamp(px, ivec2(0), screen.xy - 1);

	float t = texelFetch(albedoTex, px, 0).a;
	if (t <= 0.0) discard;
	vec4 water = texelFetch(waterTex, px, 0);

	/* Reconstruct the water surface position from the pixel's camera ray */
	vec2 ndc = (vec2(px) + 0.5) / vec2(screen.xy) * 2.0 - 1.0;
	vec4 pNear = invViewProj * vec4(ndc, -1.0, 1.0);
	vec4 pFar  = invViewProj * vec4(ndc,  1.0, 1.0);
	pNear.xyz /= pNear.w;
	pFar.xyz  /= pFar.w;
	vec3 pos = pNear.xyz + normalize(pFar.xyz - pNear.xyz) * t;

	vec4  eye = view * vec4(pos, 1.0);
	float fz  = abs(eye.z);
	float f;
	if (fogCol.w > 0.0) {
		f = exp(-fogCol.w * fz);
	} else {
		f = clamp((fogParams.x - fz) / fogParams.x, 0.0, 1.0);
	}
	vec3 colour = mix(fogCol.rgb * water.a, water.rgb, f);

	vec4 clip = viewProj * vec4(pos, 1.0);
	gl_FragDepth = clamp((clip.z / clip.w) * 0.5 + 0.5, 0.0, 1.0);
	fragColour = vec4(colour, water.a);
}
