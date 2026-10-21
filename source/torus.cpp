#include "torus.hpp"

#include <cmath>

void generateTorus(float major_radius, float minor_radius,
                   uint32_t major_segments, uint32_t minor_segments,
                   std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
	const float two_pi = 2.0f * 3.14159265358979f;

	vertices.clear();
	indices.clear();

	// Вершины: сетка по двум углам.
	// u — угол по большой окружности (вокруг оси Y),
	// v — угол по малой окружности (вокруг трубки)
	for (uint32_t i = 0; i < major_segments; ++i) {
		const float u = two_pi * float(i) / float(major_segments);

		for (uint32_t j = 0; j < minor_segments; ++j) {
			const float v = two_pi * float(j) / float(minor_segments);

			// Расстояние от оси Y до точки
			const float ring = major_radius + minor_radius * std::cos(v);

			const Vec3 position = {
				ring * std::cos(u),
				minor_radius * std::sin(v),
				ring * std::sin(u),
			};

			// Процедурный цвет из позиции в локальной системе координат:
			// каждую координату переводим из её диапазона в [0, 1]
			const float outer = major_radius + minor_radius;
			const Vec3 color = {
				0.5f + 0.5f * position.x / outer,
				0.5f + 0.5f * position.y / minor_radius,
				0.5f + 0.5f * position.z / outer,
			};

			vertices.push_back({ position, color });
		}
	}

	// Индексы: каждую клетку сетки делим на два треугольника.
	// Остаток от деления (%) замыкает сетку: после последней вершины снова идёт первая.
	// Порядок вершин подобран так, что треугольник, повёрнутый наружу к камере,
	// на экране обходится против часовой стрелки (VK_FRONT_FACE_COUNTER_CLOCKWISE)
	for (uint32_t i = 0; i < major_segments; ++i) {
		const uint32_t i_next = (i + 1) % major_segments;

		for (uint32_t j = 0; j < minor_segments; ++j) {
			const uint32_t j_next = (j + 1) % minor_segments;

			const uint32_t p00 = i * minor_segments + j;
			const uint32_t p10 = i_next * minor_segments + j;
			const uint32_t p01 = i * minor_segments + j_next;
			const uint32_t p11 = i_next * minor_segments + j_next;

			indices.push_back(p00);
			indices.push_back(p01);
			indices.push_back(p10);

			indices.push_back(p10);
			indices.push_back(p01);
			indices.push_back(p11);
		}
	}
}
