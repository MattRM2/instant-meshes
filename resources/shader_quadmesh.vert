#version 330

uniform vec3 light_position;
uniform mat4 proj, model, view;

in vec3 position;
in vec3 normal;
in vec2 texcoord;

out fData {
	vec3 to_eye;
	vec3 to_light;
	vec3 normal;
	vec2 uv;
} frag;

void main() {
	vec4 pos_camera = view * (model * vec4(position, 1.0));
	gl_Position = proj * pos_camera;
	frag.to_light = (view * vec4(light_position, 1.0)).xyz - pos_camera.xyz;
	frag.to_eye = -pos_camera.xyz;
	frag.normal = (model * (view * vec4(normal, 0.0))).xyz;
	frag.uv = texcoord;
}
