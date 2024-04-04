#include "RayTracerApplication.h"
#include "Geometry/Materials/Lambertian.h"

RayTracerApplication::RayTracerApplication(int width, int height, GLFWwindow *window)
	:m_Renderer(width, height, window), m_Scene(width, height)
{
	MaterialManager& materialManager = m_Scene.GetMaterialManager();
	Material material;
	material.materialType = Material::Type::DIFFUSE;
	material.diffuse.albedo = make_float3(0.0f, 0.1f, 0.1f);
	materialManager.AddMaterial(material);
	material.diffuse.albedo = make_float3(1.0f, 0.2f, 0.0f);
	materialManager.AddMaterial(material);
	material.diffuse.albedo = make_float3(0.21f, 0.27f, 0.65f);
	materialManager.AddMaterial(material);

	Sphere sphere = {
		1000.0f,
		make_float3(0.0f, -1000.0f, 0.0f),
		0
	};
	m_Scene.AddSphere(sphere);

	sphere = {
		0.9f,
		make_float3(0.0f, 0.9f, 0.0f),
		1
	};
	m_Scene.AddSphere(sphere);

	sphere = {
		0.62f,
		make_float3(1.4f, 0.5f, 0.0f),
		2
	};
	m_Scene.AddSphere(sphere);

	sphere = {
		0.5f,
		make_float3(0.0f, 0.5f, 1.4f),
		1
	};
	m_Scene.AddSphere(sphere);

	sphere = {
		0.5f,
		make_float3(-1.4f, 0.5f, 0.0f),
		1
	};
	m_Scene.AddSphere(sphere);
}

void RayTracerApplication::Update(float deltaTime)
{
	m_Scene.GetCamera()->OnUpdate(deltaTime);
	Display(deltaTime);
}

void RayTracerApplication::Display(float deltaTime)
{
	m_Renderer.Render(m_Scene, deltaTime);
}

void RayTracerApplication::OnResize(int width, int height)
{
}

