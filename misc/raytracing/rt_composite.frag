/* Combines the traced lighting terms into the final colour, applies fog to
   match the rasterised sky/clouds/entities, and writes depth so that
   everything the game draws afterwards is occluded correctly. */
layout(binding = 6)  uniform sampler2D gbufTex;
layout(binding = 7)  uniform sampler2D albedoTex;
layout(binding = 8)  uniform sampler2D directTex;
layout(binding = 9)  uniform sampler2D indirectTex;
layout(binding = 10) uniform sampler2D waterTex;
layout(binding = 11) uniform sampler2D normalTex;

out vec4 fragColour;

void main() {
	/* The trace buffers may be smaller than the window (rt-scale option) */
	ivec2 px = ivec2(vec2(gl_FragCoord.xy) * vec2(screen.xy) / vec2(window.xy));
	px = clamp(px, ivec2(0), screen.xy - 1);
	vec4 g = texelFetch(gbufTex, px, 0);
	if (g.w < 0.0) discard;

	vec3 albedo   = texelFetch(albedoTex,   px, 0).rgb;
	vec3 direct   = texelFetch(directTex,   px, 0).rgb;
	vec3 indirect = texelFetch(indirectTex, px, 0).rgb;
	vec3 colour   = albedo * (direct + indirect);

	/* rt-debug option: view individual lighting terms */
	int debug = int(fogParams.w);
	if (debug != 0) {
		vec4 n = texelFetch(normalTex, px, 0);
		if (debug == 1) colour = direct;
		if (debug == 2) colour = indirect;
		if (debug == 3) colour = albedo;
		if (debug == 4) colour = n.xyz * 0.5 + 0.5;
		if (debug == 5) colour = texelFetch(waterTex, px, 0).rgb;
		if (debug == 6) colour = vec3(fract(g.w / 16.0));
		if (debug == 7) colour = vec3(n.w / 64.0);
		vec4 dclip = viewProj * vec4(g.xyz, 1.0);
		gl_FragDepth = clamp((dclip.z / dclip.w) * 0.5 + 0.5, 0.0, 1.0);
		fragColour = vec4(colour, 1.0);
		return;
	}

	/* Fog, using eye space depth like the fixed function pipeline does */
	vec4  eye = view * vec4(g.xyz, 1.0);
	float fz  = abs(eye.z);
	float f;
	if (fogCol.w > 0.0) {
		f = exp(-fogCol.w * fz);
	} else {
		f = clamp((fogParams.x - fz) / fogParams.x, 0.0, 1.0);
	}
	colour = mix(fogCol.rgb, colour, f);

	vec4 clip = viewProj * vec4(g.xyz, 1.0);
	gl_FragDepth = clamp((clip.z / clip.w) * 0.5 + 0.5, 0.0, 1.0);
	fragColour = vec4(colour, 1.0);
}
