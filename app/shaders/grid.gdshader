// Blueprint floor: blue with a 1 m grid and a stronger 10 m grid, fading with distance.
shader_type spatial;
render_mode unshaded, cull_disabled;

uniform vec3 base_color : source_color = vec3(0.10, 0.25, 0.50);
uniform vec3 line_color : source_color = vec3(0.80, 0.90, 1.00);
uniform float fade_distance = 150.0;

varying vec3 world_pos;

void vertex() {
    world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

float grid(vec2 p, float step, float width) {
    vec2 g = abs(fract(p / step - 0.5) - 0.5) / fwidth(p / step);
    float line = min(g.x, g.y);
    return 1.0 - clamp(line / width, 0.0, 1.0);
}

void fragment() {
    vec2 p = world_pos.xz;
    float g1 = grid(p, 1.0, 1.0) * 0.35;
    float g10 = grid(p, 10.0, 1.5) * 0.9;
    float d = length(world_pos.xz - CAMERA_POSITION_WORLD.xz);
    float fade = clamp(1.0 - d / fade_distance, 0.0, 1.0);
    vec3 col = mix(base_color, line_color, max(g1, g10) * fade);
    ALBEDO = col;
}
