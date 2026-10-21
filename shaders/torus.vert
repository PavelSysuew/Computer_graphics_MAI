#version 450 core

// Входы: атрибуты вершины из вершинного буфера (номера совпадают с location в VkVertexInputAttributeDescription)
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;

// Набор 0 — общие данные сцены (один на всю сцену)
layout(std140, set = 0, binding = 0) uniform SceneUniforms {
	// Произведение матриц проекции и вида: P * V
	mat4 view_projection;
} scene_uniforms;

// Набор 1 — данные конкретного объекта (у каждого тора свой)
layout(std140, set = 1, binding = 0) uniform ModelUniforms {
	// Матрица модели M: ставит объект в мир
	mat4 model;
	// Цвет, выбранный в интерфейсе
	vec4 color;
	// 1 — умножать на цвета вершин, 0 — не использовать их
	float use_vertex_colors;
} model_uniforms;

// Цвет вершины для фрагментного шейдера
layout(location = 0) out vec3 out_color;

void main() {
	// p' = P * V * M * p (лекция 4, MVP)
	gl_Position = scene_uniforms.view_projection * model_uniforms.model * vec4(in_position, 1.0);

	// mix(a, b, t) = a * (1 - t) + b * t: при 0 берём белый, при 1 — цвет вершины
	vec3 vertex_color = mix(vec3(1.0), in_color, model_uniforms.use_vertex_colors);

	// Выбранный цвет умножается на цвет вершины (доп. задание 5)
	out_color = vertex_color * model_uniforms.color.rgb;
}
