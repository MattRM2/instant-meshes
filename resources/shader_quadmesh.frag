#version 330
precision lowp float;

in fData {
	vec3 to_eye;
	vec3 to_light;
	vec3 normal;
	vec2 uv;
} frag;

uniform float textured;     // 1: the UV checker, repeated 'tiling' times
uniform float tiling;
uniform sampler2D tex;

out vec4 outColor;

void main() {
	vec3 to_light = normalize(frag.to_light);
	vec3 to_eye = normalize(frag.to_eye);
	vec3 normal = normalize(frag.normal);
	vec3 refl = reflect(-to_light, normal);

	float diffuse_factor = max(0.0, dot(to_light, normal));
	float specular_factor = pow(max(dot(to_eye, refl), 0.0), 10.0);

	if (textured > 0.5) {
		// the image's first row is the top of the UV square (v = 1)
		vec3 Kd = texture(tex, vec2(frag.uv.x, 1.0 - frag.uv.y) * tiling).rgb;
		outColor = vec4(Kd * (0.45 + 0.55 * diffuse_factor) + vec3(0.12) * specular_factor, 1.0);
		return;
	}

	vec3 Kd = vec3(0.46, 0.46, 0.46);   // Matt Dark clay
	vec3 Ks = vec3(1.0);
	vec3 Ka = Kd * 0.2;
	outColor = vec4(Ka + Kd*diffuse_factor + Ks*specular_factor, 1.0f);
}
