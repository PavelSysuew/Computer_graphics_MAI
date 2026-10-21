#pragma once

#include <cstdint>
#include <vector>

#include "linear_math.hpp"

// Одна вершина: позиция и цвет (interleaved — атрибуты лежат подряд, лекция 3)
struct Vertex {
	Vec3 position;
	Vec3 color;
};

// Генерирует сетку тора.
// major_radius — радиус большой окружности (от центра тора до центра трубки),
// minor_radius — радиус трубки,
// major_segments, minor_segments — на сколько частей делим большую и малую окружности
void generateTorus(float major_radius, float minor_radius,
                   uint32_t major_segments, uint32_t minor_segments,
                   std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
