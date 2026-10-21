#pragma once

#include <cmath>

// Вектор из трёх чисел (x, y, z)
struct Vec3 {
	float x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

// Матрица 4x4. Хранится по столбцам (column-major), как mat4 в GLSL,
// поэтому её можно копировать в uniform-буфер без перестановки
struct Mat4 {
	// Элемент в строке row и столбце col лежит в m[col * 4 + row]
	float m[16];

	float& at(int row, int col) { return m[col * 4 + row]; }
	float at(int row, int col) const { return m[col * 4 + row]; }
};

// Перевод градусов в радианы (тригонометрия в C++ работает с радианами)
inline float radians(float degrees) {
	return degrees * 3.14159265358979f / 180.0f;
}

// Единичная матрица: ничего не меняет
inline Mat4 identity() {
	Mat4 r = {};
	r.at(0, 0) = 1.0f;
	r.at(1, 1) = 1.0f;
	r.at(2, 2) = 1.0f;
	r.at(3, 3) = 1.0f;
	return r;
}

// Произведение матриц. В (a * b) * p сначала применяется b, потом a
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
	Mat4 r = {};
	for (int row = 0; row < 4; ++row) {
		for (int col = 0; col < 4; ++col) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) {
				sum += a.at(row, k) * b.at(k, col);
			}
			r.at(row, col) = sum;
		}
	}
	return r;
}

// Матрица переноса: единичная со сдвигом (tx, ty, tz) в последнем столбце (лекция 1)
inline Mat4 translation(Vec3 t) {
	Mat4 r = identity();
	r.at(0, 3) = t.x;
	r.at(1, 3) = t.y;
	r.at(2, 3) = t.z;
	return r;
}

// Матрица растяжения: коэффициенты по диагонали
inline Mat4 scaling(Vec3 s) {
	Mat4 r = identity();
	r.at(0, 0) = s.x;
	r.at(1, 1) = s.y;
	r.at(2, 2) = s.z;
	return r;
}

// Поворот вокруг оси X на угол angle (в радианах)
inline Mat4 rotationX(float angle) {
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	Mat4 r = identity();
	r.at(1, 1) = c;
	r.at(1, 2) = -s;
	r.at(2, 1) = s;
	r.at(2, 2) = c;
	return r;
}

// Поворот вокруг оси Y на угол angle (в радианах)
inline Mat4 rotationY(float angle) {
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	Mat4 r = identity();
	r.at(0, 0) = c;
	r.at(0, 2) = s;
	r.at(2, 0) = -s;
	r.at(2, 2) = c;
	return r;
}

// Поворот вокруг оси Z на угол angle (в радианах)
inline Mat4 rotationZ(float angle) {
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	Mat4 r = identity();
	r.at(0, 0) = c;
	r.at(0, 1) = -s;
	r.at(1, 0) = s;
	r.at(1, 1) = c;
	return r;
}

// Перспективная проекция через вертикальный угол обзора (лекция 1).
// Камера смотрит вдоль +z, глубина после деления на w попадает в [0, 1]
inline Mat4 perspective(float fov_y, float aspect, float near_plane, float far_plane) {
	const float t = std::tan(fov_y / 2.0f);
	Mat4 r = {};
	r.at(0, 0) = 1.0f / (aspect * t);
	r.at(1, 1) = 1.0f / t;
	r.at(2, 2) = far_plane / (far_plane - near_plane);
	r.at(2, 3) = -far_plane * near_plane / (far_plane - near_plane);
	// w := z, потом видеокарта делит x, y, z на w (перспективное деление)
	r.at(3, 2) = 1.0f;
	return r;
}

// Ортографическая проекция (лекция 1). Объём вида — параллелепипед:
// по y от -half_height до half_height, по x с учётом соотношения сторон окна
inline Mat4 orthographic(float half_height, float aspect, float near_plane, float far_plane) {
	const float t = -half_height;
	const float b = half_height;
	const float l = -half_height * aspect;
	const float r_ = half_height * aspect;

	Mat4 r = identity();
	r.at(0, 0) = 2.0f / (r_ - l);
	r.at(0, 3) = -(r_ + l) / (r_ - l);
	r.at(1, 1) = 2.0f / (b - t);
	r.at(1, 3) = -(b + t) / (b - t);
	r.at(2, 2) = 1.0f / (far_plane - near_plane);
	r.at(2, 3) = -near_plane / (far_plane - near_plane);
	return r;
}
