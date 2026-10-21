#include "application.hpp"

#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fstream>
#include <iostream>
#include <vector>

#include <imgui.h>

#include "linear_math.hpp"
#include "torus.hpp"

namespace application {

namespace {

using graphics::internal::context;

// Буфер в памяти видеокарты вместе с выделенной под него памятью
struct Buffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	// Адрес, по которому процессор может писать в этот буфер
	void* mapped;
};

// Данные набора 0 (как SceneUniforms в шейдере)
struct SceneUniforms {
	Mat4 view_projection;
};

// Данные набора 1 (как ModelUniforms в шейдере). Раскладка std140:
// mat4 занимает 64 байта, vec4 — 16, float — 4
struct ModelUniforms {
	Mat4 model;
	float color[4];
	float use_vertex_colors;
	float padding[3];
};

// Один тор на сцене: параметры из интерфейса и свои ресурсы Vulkan
struct Object {
	bool visible;

	// Положение, поворот (в градусах) и растяжение
	Vec3 position;
	Vec3 rotation;
	Vec3 scale;

	// Цвет и флаг «умножать на цвета вершин»
	float color[3];
	bool use_vertex_colors;

	// Анимация: движение по траектории-«восьмёрке» с покачиванием вверх-вниз и вращение
	bool animation_playing;
	float animation_speed;
	float trajectory_radius;
	float trajectory_height;
	float spin_speed;
	float animation_time;

	// Свой uniform-буфер и свой набор дескрипторов (доп. задание 6)
	Buffer uniform_buffer;
	VkDescriptorSet descriptor_set;
};

constexpr uint32_t object_count = 3;

// Параметры сетки тора
constexpr float torus_major_radius = 1.0f;
constexpr float torus_minor_radius = 0.4f;
constexpr uint32_t torus_major_segments = 48;
constexpr uint32_t torus_minor_segments = 24;

VkDescriptorSetLayout vk_scene_set_layout;
VkDescriptorSetLayout vk_model_set_layout;
VkPipelineLayout vk_pipeline_layout;
VkPipeline vk_pipeline;
VkDescriptorPool vk_descriptor_pool;

Buffer vertex_buffer;
Buffer index_buffer;
uint32_t index_count;

Buffer scene_uniform_buffer;
VkDescriptorSet vk_scene_descriptor_set;

Object objects[object_count];
int selected_object = 0;

// Проекция
bool use_perspective = true;
float fov_degrees = 60.0f;
float ortho_half_height = 4.0f;
float near_plane = 0.1f;
float far_plane = 100.0f;

// Камера вращается вокруг начала координат на расстоянии camera_distance
float camera_distance = 9.0f;
float camera_yaw = 0.0f;
float camera_pitch = -25.0f;

double last_time = 0.0;

// Значения объекта по умолчанию
Object makeObject(Vec3 position, Vec3 color, bool playing) {
	Object object = {};
	object.visible = true;
	object.position = position;
	object.rotation = { 0.0f, 0.0f, 0.0f };
	object.scale = { 1.0f, 1.0f, 1.0f };
	object.color[0] = color.x;
	object.color[1] = color.y;
	object.color[2] = color.z;
	object.use_vertex_colors = true;
	object.animation_playing = playing;
	object.animation_speed = 1.0f;
	object.trajectory_radius = 1.5f;
	object.trajectory_height = 0.5f;
	object.spin_speed = 60.0f;
	object.animation_time = 0.0f;
	return object;
}

// Создаёт буфер, к которому есть доступ с процессора (лекция 3, загрузка через VMA)
bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& result) {
	// Анкета буфера
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		// Размер в байтах
		.size = size,
		// Для чего буфер: вершины, индексы или uniform-данные
		.usage = usage,
		// Буфер использует только одна очередь
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	// Анкета выделения памяти для VMA
	const VmaAllocationCreateInfo allocation_info = {
		// MAPPED — сразу получить адрес для записи с процессора,
		// HOST_ACCESS_SEQUENTIAL_WRITE — процессор пишет в память последовательно
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		// VMA сама выбирает подходящий тип памяти
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	// Сюда VMA запишет сведения о выделенной памяти, в том числе адрес для записи
	VmaAllocationInfo info = {};

	if (vmaCreateBuffer(context.allocator, &buffer_info, &allocation_info,
	                    &result.buffer, &result.allocation, &info) != VK_SUCCESS) {
		std::cerr << "Failed to create Vulkan buffer\n";
		return false;
	}

	result.mapped = info.pMappedData;
	return true;
}

void destroyBuffer(Buffer& buffer) {
	vmaDestroyBuffer(context.allocator, buffer.buffer, buffer.allocation);
	buffer = {};
}

// Читает скомпилированный шейдер (.spv) с диска и создаёт из него VkShaderModule
VkShaderModule loadShaderModule(const char* path) {
	// Открываем файл в двоичном режиме, сразу в конце (ate), чтобы узнать размер
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		std::cerr << "Failed to open shader file " << path << '\n';
		return VK_NULL_HANDLE;
	}

	const size_t size = size_t(file.tellg());

	// SPIR-V состоит из 32-битных слов, поэтому храним его в массиве uint32_t
	std::vector<uint32_t> code(size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(code.data()), std::streamsize(size));

	// Анкета шейдерного модуля
	const VkShaderModuleCreateInfo module_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		// Размер кода в байтах
		.codeSize = size,
		// Указатель на код
		.pCode = code.data(),
	};

	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(context.device, &module_info, nullptr, &module) != VK_SUCCESS) {
		std::cerr << "Failed to create shader module from " << path << '\n';
		return VK_NULL_HANDLE;
	}

	return module;
}

// Генерирует тор и копирует его в вершинный и индексный буферы
bool createMesh() {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	generateTorus(torus_major_radius, torus_minor_radius,
	              torus_major_segments, torus_minor_segments, vertices, indices);

	const VkDeviceSize vertices_size = vertices.size() * sizeof(Vertex);
	const VkDeviceSize indices_size = indices.size() * sizeof(uint32_t);

	if (!createBuffer(vertices_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer)) {
		return false;
	}

	if (!createBuffer(indices_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer)) {
		return false;
	}

	// Копируем данные из оперативной памяти в буферы
	std::memcpy(vertex_buffer.mapped, vertices.data(), vertices_size);
	std::memcpy(index_buffer.mapped, indices.data(), indices_size);

	index_count = uint32_t(indices.size());
	return true;
}

// Создаёт раскладку набора дескрипторов с одним uniform-буфером в binding 0
bool createUniformSetLayout(VkDescriptorSetLayout& layout) {
	// Описание одного дескриптора
	const VkDescriptorSetLayoutBinding binding = {
		// Номер binding в шейдере
		.binding = 0,
		// Тип ресурса: uniform-буфер
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		// Один буфер, не массив
		.descriptorCount = 1,
		// Какие шейдеры его читают
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
	};

	// Анкета раскладки набора
	const VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};

	if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
		std::cerr << "Failed to create descriptor set layout\n";
		return false;
	}

	return true;
}

// Связывает буфер с binding 0 набора дескрипторов
void writeUniformDescriptor(VkDescriptorSet set, VkBuffer buffer, VkDeviceSize range) {
	// Какой буфер и какую его часть увидит шейдер
	const VkDescriptorBufferInfo buffer_info = {
		.buffer = buffer,
		.offset = 0,
		.range = range,
	};

	// Анкета записи в набор
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		// В какой набор пишем
		.dstSet = set,
		// В какой binding
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &buffer_info,
	};

	vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
}

// Создаёт раскладки, пул и наборы дескрипторов, а также uniform-буферы
bool createDescriptors() {
	if (!createUniformSetLayout(vk_scene_set_layout) ||
	    !createUniformSetLayout(vk_model_set_layout)) {
		return false;
	}

	// Нужно 1 + object_count наборов, в каждом один uniform-буфер
	const VkDescriptorPoolSize pool_size = {
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1 + object_count,
	};

	// Анкета пула дескрипторов
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		// Сколько наборов максимум можно выделить
		.maxSets = 1 + object_count,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};

	if (vkCreateDescriptorPool(context.device, &pool_info, nullptr, &vk_descriptor_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create descriptor pool\n";
		return false;
	}

	// Набор 0: данные сцены
	const VkDescriptorSetAllocateInfo scene_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk_descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk_scene_set_layout,
	};

	if (vkAllocateDescriptorSets(context.device, &scene_allocate_info,
	                             &vk_scene_descriptor_set) != VK_SUCCESS) {
		std::cerr << "Failed to allocate scene descriptor set\n";
		return false;
	}

	if (!createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, scene_uniform_buffer)) {
		return false;
	}

	writeUniformDescriptor(vk_scene_descriptor_set, scene_uniform_buffer.buffer, sizeof(SceneUniforms));

	// Набор 1: у каждого объекта свой набор и свой буфер
	for (Object& object : objects) {
		const VkDescriptorSetAllocateInfo model_allocate_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &vk_model_set_layout,
		};

		if (vkAllocateDescriptorSets(context.device, &model_allocate_info,
		                             &object.descriptor_set) != VK_SUCCESS) {
			std::cerr << "Failed to allocate model descriptor set\n";
			return false;
		}

		if (!createBuffer(sizeof(ModelUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                  object.uniform_buffer)) {
			return false;
		}

		writeUniformDescriptor(object.descriptor_set, object.uniform_buffer.buffer, sizeof(ModelUniforms));
	}

	return true;
}

// Создаёт графический конвейер (лекция 2, стадии конвейера)
bool createPipeline() {
	// Раскладка конвейера: набор 0 — сцена, набор 1 — объект
	const VkDescriptorSetLayout set_layouts[] = {
		vk_scene_set_layout,
		vk_model_set_layout,
	};

	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = set_layouts,
	};

	if (vkCreatePipelineLayout(context.device, &layout_info, nullptr, &vk_pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create pipeline layout\n";
		return false;
	}

	VkShaderModule vertex_module = loadShaderModule("shaders/torus.vert.spv");
	VkShaderModule fragment_module = loadShaderModule("shaders/torus.frag.spv");
	if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE) {
		vkDestroyShaderModule(context.device, vertex_module, nullptr);
		vkDestroyShaderModule(context.device, fragment_module, nullptr);
		return false;
	}

	// 1. Шейдеры: вершинный и фрагментный, точка входа — функция main
	const VkPipelineShaderStageCreateInfo stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_module,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_module,
			.pName = "main",
		},
	};

	// 2. Откуда брать вершины: один буфер (binding 0), шаг — размер одной вершины
	const VkVertexInputBindingDescription vertex_binding = {
		.binding = 0,
		.stride = sizeof(Vertex),
		// Переходить к следующему элементу на каждой вершине
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};

	// Атрибуты вершины: location в шейдере, тип и смещение внутри структуры Vertex
	const VkVertexInputAttributeDescription vertex_attributes[] = {
		{
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, position),
		},
		{
			.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, color),
		},
	};

	const VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &vertex_binding,
		.vertexAttributeDescriptionCount = 2,
		.pVertexAttributeDescriptions = vertex_attributes,
	};

	// 3. Каждые три индекса — отдельный треугольник
	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	// 4. Один viewport и один scissor, сами значения задаются в render()
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	// 5. Растеризация: закрашиваем треугольники и отбрасываем задние грани.
	// Лицевые — те, что на экране обходятся против часовой стрелки (так построен тор)
	const VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	// 6. Без сглаживания (одна выборка на пиксель)
	const VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	// 7. Тест глубины: рисуем пиксель, только если он ближе уже нарисованного
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};

	// 8. Пишем все четыре канала цвета, смешивание (прозрачность) выключено
	const VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};

	const VkPipelineColorBlendStateCreateInfo color_blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color_blend_attachment,
	};

	// 9. Viewport и scissor задаются командами при записи кадра (размер окна может меняться)
	const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};

	// Анкета конвейера: собираем все стадии вместе
	const VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisample,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blend,
		.pDynamicState = &dynamic_state,
		// 10. Какие ресурсы видят шейдеры
		.layout = vk_pipeline_layout,
		// В каком render pass и подпроходе будет работать конвейер
		.renderPass = context.render_pass,
		.subpass = 0,
	};

	const VkResult result = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1,
	                                                  &pipeline_info, nullptr, &vk_pipeline);

	// Модули нужны только при создании конвейера, потом их можно удалить
	vkDestroyShaderModule(context.device, vertex_module, nullptr);
	vkDestroyShaderModule(context.device, fragment_module, nullptr);

	if (result != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline\n";
		return false;
	}

	return true;
}

// Смещение по траектории: «восьмёрка» в плоскости XZ и покачивание по Y
Vec3 trajectoryOffset(const Object& object) {
	const float t = object.animation_time;
	return {
		object.trajectory_radius * std::sin(t),
		object.trajectory_height * std::sin(3.0f * t),
		object.trajectory_radius * std::sin(t) * std::cos(t),
	};
}

// Матрица модели M = T * R * S: сначала растяжение, потом поворот, потом перенос (лекция 4)
Mat4 modelMatrix(const Object& object) {
	// Во время анимации объект ещё и вращается вокруг своих осей
	const float spin = object.spin_speed * object.animation_time;

	const Mat4 t = translation(object.position + trajectoryOffset(object));
	const Mat4 r = rotationY(radians(object.rotation.y + spin)) *
	               rotationX(radians(object.rotation.x + 0.5f * spin)) *
	               rotationZ(radians(object.rotation.z));
	const Mat4 s = scaling(object.scale);

	return t * r * s;
}

// Матрица вида V. Камера C = Ry(yaw) * Rx(pitch) * T(0, 0, -distance) летает вокруг центра
// и смотрит на него. V = C^-1 = T(0, 0, distance) * Rx(-pitch) * Ry(-yaw) (лекция 4)
Mat4 viewMatrix() {
	return translation({ 0.0f, 0.0f, camera_distance }) *
	       rotationX(radians(-camera_pitch)) *
	       rotationY(radians(-camera_yaw));
}

Mat4 projectionMatrix(float aspect) {
	if (use_perspective) {
		return perspective(radians(fov_degrees), aspect, near_plane, far_plane);
	}
	return orthographic(ortho_half_height, aspect, near_plane, far_plane);
}

// Окно интерфейса
void drawInterface() {
	ImGui::Begin("Lab 1: Torus");

	// Доп. задание 1: переключение проекции
	if (ImGui::CollapsingHeader("Projection", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::RadioButton("Perspective", use_perspective)) {
			use_perspective = true;
		}
		ImGui::SameLine();
		if (ImGui::RadioButton("Orthographic", !use_perspective)) {
			use_perspective = false;
		}

		if (use_perspective) {
			ImGui::SliderFloat("FOV", &fov_degrees, 20.0f, 120.0f, "%.0f deg");
		} else {
			ImGui::SliderFloat("Half height", &ortho_half_height, 1.0f, 20.0f);
		}
		ImGui::SliderFloat("Near", &near_plane, 0.01f, 5.0f);
		ImGui::SliderFloat("Far", &far_plane, 10.0f, 200.0f);
	}

	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Distance", &camera_distance, 2.0f, 30.0f);
		ImGui::SliderFloat("Yaw", &camera_yaw, -180.0f, 180.0f, "%.0f deg");
		ImGui::SliderFloat("Pitch", &camera_pitch, -89.0f, 89.0f, "%.0f deg");
	}

	// Выбор объекта, который настраиваем ниже
	ImGui::SeparatorText("Objects");
	for (uint32_t i = 0; i < object_count; ++i) {
		ImGui::PushID(int(i));
		ImGui::Checkbox("##visible", &objects[i].visible);
		ImGui::SameLine();
		char label[32];
		std::snprintf(label, sizeof(label), "Torus %u", i + 1);
		ImGui::RadioButton(label, &selected_object, int(i));
		ImGui::PopID();
		if (i + 1 < object_count) {
			ImGui::SameLine();
		}
	}

	Object& object = objects[selected_object];

	// Доп. задание 2: положение, поворот, растяжение
	if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::DragFloat3("Position", &object.position.x, 0.05f);
		ImGui::SliderFloat3("Rotation", &object.rotation.x, -180.0f, 180.0f, "%.0f deg");
		ImGui::DragFloat3("Scale", &object.scale.x, 0.01f, 0.1f, 5.0f);
		if (ImGui::Button("Reset transform")) {
			object.rotation = { 0.0f, 0.0f, 0.0f };
			object.scale = { 1.0f, 1.0f, 1.0f };
		}
	}

	// Доп. задания 4 и 5: цвет объекта и цвета вершин
	if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3("Color", object.color);
		ImGui::Checkbox("Multiply by vertex colors", &object.use_vertex_colors);
	}

	// Доп. задание 3: анимация по траектории
	if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button(object.animation_playing ? "Pause" : "Play")) {
			object.animation_playing = !object.animation_playing;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset animation")) {
			object.animation_time = 0.0f;
		}
		ImGui::SliderFloat("Speed", &object.animation_speed, 0.0f, 5.0f);
		ImGui::SliderFloat("Radius", &object.trajectory_radius, 0.0f, 5.0f);
		ImGui::SliderFloat("Height", &object.trajectory_height, 0.0f, 3.0f);
		ImGui::SliderFloat("Spin speed", &object.spin_speed, -360.0f, 360.0f, "%.0f deg/s");
	}

	ImGui::End();
}

} // namespace

bool initialize() {
	objects[0] = makeObject({ 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, false);
	objects[1] = makeObject({ -3.5f, 0.0f, 0.0f }, { 1.0f, 0.6f, 0.3f }, true);
	objects[2] = makeObject({ 3.5f, 0.0f, 0.0f }, { 0.4f, 0.8f, 1.0f }, true);
	objects[2].rotation = { 90.0f, 0.0f, 0.0f };
	objects[2].animation_speed = 0.6f;
	objects[2].spin_speed = -90.0f;

	if (!createMesh() || !createDescriptors() || !createPipeline()) {
		return false;
	}

	return true;
}

void shutdown() {
	vkQueueWaitIdle(context.graphics_queue);

	vkDestroyPipeline(context.device, vk_pipeline, nullptr);
	vkDestroyPipelineLayout(context.device, vk_pipeline_layout, nullptr);

	// Наборы дескрипторов удаляются вместе с пулом
	vkDestroyDescriptorPool(context.device, vk_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_model_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_scene_set_layout, nullptr);

	for (Object& object : objects) {
		destroyBuffer(object.uniform_buffer);
	}
	destroyBuffer(scene_uniform_buffer);
	destroyBuffer(index_buffer);
	destroyBuffer(vertex_buffer);
}

void update(double time) {
	// Сколько секунд прошло с прошлого кадра
	float dt = float(time - last_time);
	last_time = time;
	if (dt > 0.1f) {
		dt = 0.1f;
	}

	// Время анимации идёт, только когда она не на паузе
	for (Object& object : objects) {
		if (object.animation_playing) {
			object.animation_time += dt * object.animation_speed;
		}
	}

	drawInterface();
}

void render(const graphics::internal::FrameData& fd) {

	// Короткое им для командного буфера
	VkCommandBuffer cb = fd.command_buffer;

	// Записываем uniform-данные здесь, а не в update(): к этому моменту prepare()
	// уже дождался, пока видеокарта закончит прошлый кадр и перестанет читать буферы
	const VkExtent2D extent = context.swapchain_extent;
	const float aspect = float(extent.width) / float(extent.height);

	const SceneUniforms scene = {
		.view_projection = projectionMatrix(aspect) * viewMatrix(),
	};
	std::memcpy(scene_uniform_buffer.mapped, &scene, sizeof(scene));

	for (Object& object : objects) {
		ModelUniforms model = {
			.model = modelMatrix(object),
			.color = { object.color[0], object.color[1], object.color[2], 1.0f },
			.use_vertex_colors = object.use_vertex_colors ? 1.0f : 0.0f,
		};
		std::memcpy(object.uniform_buffer.mapped, &model, sizeof(model));
	}

	// Стираем записку с прошлого кадра
	vkResetCommandBuffer(cb, 0);

	// Анкета для начала записи командного буфера
	const VkCommandBufferBeginInfo begin_info = {
		// Сообщает вулкану, какая это анкета
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		// Значит, что мы будем использовать этот командный буфер только один раз
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	// Начинаем запись командного буфера
	vkBeginCommandBuffer(cb, &begin_info);

	// Значения очистки буфера кадра (цвет и глубина)
	const VkClearValue clear_values[] = {
		{ .color = { { 0.1f, 0.1f, 0.1f, 1.0f } } }, // цвет фона и прозрачность
		{ .depthStencil = { 1.0f, 0 } }, // для буфера глубины
	};

	// Анкета для начала прохода рисования
	const VkRenderPassBeginInfo render_pass_begin = {
		// Тип анкеты
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		// Какой проход начать
		.renderPass = context.render_pass,
		// В какие конкретные картинки рисовать
		.framebuffer = fd.framebuffer,
		// Прямоугольник, в котором идёт рисование
		.renderArea = { .extent = extent },
		// Сколько значений очистки передаём
		.clearValueCount = 2,
		// Указатель на массив значений очистки
		.pClearValues = clear_values,
	};

	// Записывает в командный буфер команду начала прохода рисования
	vkCmdBeginRenderPass(cb, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

	// Viewport: переводит координаты [-1, 1] в пиксели окна, глубину — в [0, 1]
	const VkViewport viewport = {
		.x = 0.0f,
		.y = 0.0f,
		.width = float(extent.width),
		.height = float(extent.height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	vkCmdSetViewport(cb, 0, 1, &viewport);

	// Scissor: всё, что вне этого прямоугольника, отрезается
	const VkRect2D scissor = { .extent = extent };
	vkCmdSetScissor(cb, 0, 1, &scissor);

	// Подключаем конвейер, вершины и индексы тора (они общие для всех объектов)
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);

	const VkDeviceSize vertex_offset = 0;
	vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer.buffer, &vertex_offset);
	vkCmdBindIndexBuffer(cb, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

	// Набор 0 (сцена) подключаем один раз
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
	                        0, 1, &vk_scene_descriptor_set, 0, nullptr);

	// Для каждого объекта подключаем его набор 1 и рисуем
	for (const Object& object : objects) {
		if (!object.visible) {
			continue;
		}

		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
		                        1, 1, &object.descriptor_set, 0, nullptr);
		vkCmdDrawIndexed(cb, index_count, 1, 0, 0, 0);
	}

	// Записывает в командный буфер команду "закончить проход рисования"
	vkCmdEndRenderPass(cb);

	// Заканчиваем запись командного буфера
	vkEndCommandBuffer(cb);
}

} // namespace application
