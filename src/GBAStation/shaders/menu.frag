#version 450

layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(set = 0, binding = 1) uniform sampler2D border_gradient;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_textured;

layout(location = 0) out vec4 out_color;

void main() {
    if (in_textured < 0.5) {
        out_color = in_color;
    } else if (in_textured < 1.5) {
        float coverage = texture(atlas, in_uv).r;
        out_color = vec4(in_color.rgb, in_color.a * coverage);
    } else {
        out_color = texture(border_gradient, in_uv) * in_color;
    }
}
