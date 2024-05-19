#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "Renderer.h"
#include "Cuda/PathTracer.cuh"
#include "Utils/Utils.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "FileDialog.h"


Renderer::Renderer(uint32_t width, uint32_t height, GLFWwindow* window, Scene* scene)
	:m_ViewportWidth(width), m_ViewportHeight(height), m_Scene(scene), m_HierarchyPannel(scene)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.FontDefault = io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/OpenSans-Regular.ttf", 16.0f);
    ImGui::StyleColorsCustomDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	m_PixelBuffer = std::make_shared<PixelBuffer>(width, height);
	m_Texture = std::make_shared<OGLTexture>(width, height);

	checkCudaErrors(cudaMalloc((void**)&m_AccumulationBuffer, width * height * sizeof(float3)));

	m_DisplayFPSTimer = glfwGetTime();
}

Renderer::~Renderer()
{
	checkCudaErrors(cudaFree((void*)m_AccumulationBuffer));
	ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Renderer::Reset()
{
	m_FrameNumber = 0;
	m_MRPS = 0;
	m_NumRaysProcessed = 0;
	m_PixelBuffer = std::make_shared<PixelBuffer>(m_ViewportWidth, m_ViewportHeight);
	checkCudaErrors(cudaMalloc((void**)&m_AccumulationBuffer, m_ViewportWidth * m_ViewportHeight * sizeof(float3)));
	m_DisplayFPSTimer = glfwGetTime();
}

void Renderer::Render(Scene& scene, float deltaTime)
{ 
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	UpdateTimer(deltaTime);

	// Position UI and resize the texture and pixel buffer depending on the viewport size
	RenderUI(scene);

	if (scene.SendDataToDevice())
		m_FrameNumber = 0;

	// Launch cuda path tracing kernel, writes the viewport into the pixelbuffer
	if (!scene.IsEmpty())
	{
		m_FrameNumber++;

		RenderViewport(m_PixelBuffer, m_FrameNumber, m_AccumulationBuffer);

		// Unpack the pixel buffer written by cuda to the renderer texture
		UnpackToTexture();

	}
	else
		m_FrameNumber = 0;


	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Renderer::RenderUI(Scene& scene)
{
	ImGui::DockSpaceOverViewport();

	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Open...", "Ctrl+O"))
			{
				std::string fullPath = FileDialog::OpenFile(
					"3D model (*.obj;*.ply;*.stl;*.glb;*.gltf;*.fbx;*.3ds;*.blend;*.dae)\0*.obj;*.ply;*.stl;*.glb;*.gltf;*.fbx;*.3ds;*.x3d;*.blend;*.dae\0"
				);
				if (!fullPath.empty())
				{
					checkCudaErrors(cudaDeviceSynchronize());
					checkCudaErrors(cudaDeviceReset());
					Reset();
					scene.Reset();

					std::string fileName, filePath;
					Utils::GetPathAndFileName(fullPath, filePath, fileName);
					scene.CreateMeshInstanceFromFile(filePath, fileName);
					checkCudaErrors(cudaDeviceSynchronize());
				}
			}

			if (ImGui::MenuItem("Load HDR map", "Ctrl+H"))
			{
				std::string fullPath = FileDialog::OpenFile(
					"HDR file (*.hdr)\0*.hdr;*.exr\0"
				);
				if (!fullPath.empty())
				{
					std::string fileName, filePath;
					Utils::GetPathAndFileName(fullPath, filePath, fileName);
					scene.AddHDRMap(filePath, fileName);
					m_FrameNumber = 0;
				}
			}

			if (ImGui::MenuItem("Save Screenshot", "Ctrl+S")) {
				SaveScreenshot();
			}

			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("Viewport");

	if (ImGui::IsWindowFocused() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !scene.IsEmpty())
	{
		ImVec2 viewportPos = ImGui::GetCursorScreenPos();
		ImVec2 mousePos = ImGui::GetMousePos();
		int2 hoveredPixel = make_int2(mousePos.x - viewportPos.x, mousePos.y - viewportPos.y);
		hoveredPixel.y = m_ViewportHeight - hoveredPixel.y;

		if (hoveredPixel.x >= 0 && hoveredPixel.x < m_ViewportWidth && hoveredPixel.y >= 0 && hoveredPixel.y < m_ViewportHeight)
		{
			std::shared_ptr<Camera> camera = scene.GetCamera();
			Ray ray = camera->RayThroughPixel(hoveredPixel);
			std::shared_ptr<TLAS> tlas = scene.GetTLAS();
			tlas->Intersect(ray);
			m_HierarchyPannel.SetSelectionContext(ray.hit.instanceIdx);
		}
	}


	uint32_t viewportWidth = ImGui::GetContentRegionAvail().x;
	uint32_t viewportHeight = ImGui::GetContentRegionAvail().y;

	scene.GetCamera()->OnResize(viewportWidth, viewportHeight);
	OnResize(viewportWidth, viewportHeight);

	ImGui::Image((void *)(intptr_t)m_Texture->GetHandle(), ImVec2(m_Texture->GetWidth(), m_Texture->GetHeight()), ImVec2(0, 1), ImVec2(1, 0));

	ImGui::End();
	ImGui::PopStyleVar();
	
	m_HierarchyPannel.OnImGuiRender();

	ImGui::Begin("Settings");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Text("Time info");
	ImGui::Text("Render time millisec: %.3f", m_DeltaTime);
	ImGui::Text("FPS: %d", (int)(1000.0f / m_DeltaTime));
	ImGui::Text("Frame: %d", m_FrameNumber);
	ImGui::Text("Megarays/sec: %.2f", m_MRPS);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Text("Camera");
	if (ImGui::SliderFloat("Field of view", &scene.GetCamera()->GetVerticalFOV(), 1.0f, 180.0f))
		scene.GetCamera()->Invalidate();
	if (ImGui::DragFloat("Focus distance", &scene.GetCamera()->GetFocusDist(), 0.02f, 0.01f, 1000.0f))
		scene.GetCamera()->Invalidate();
	if (ImGui::DragFloat("Defocus angle", &scene.GetCamera()->GetDefocusAngle(), 0.2f, 0.0f, 180.0f))
		scene.GetCamera()->Invalidate();

	ImGui::End();

}

void Renderer::UpdateTimer(float deltaTime)
{
	m_NAccumulatedFrame++;
	m_NumRaysProcessed += m_ViewportHeight * m_ViewportWidth;

	m_AccumulatedTime += deltaTime;
	if (glfwGetTime() - m_DisplayFPSTimer >= 0.2f || m_DeltaTime == 0)
	{
		m_DisplayFPSTimer = glfwGetTime();
		m_DeltaTime = m_AccumulatedTime / m_NAccumulatedFrame;
		m_MRPS = static_cast<float>(m_NumRaysProcessed) / m_AccumulatedTime / 1000.0f;		// millisecond * 1.000.000
		
		m_NAccumulatedFrame = 0;
		m_AccumulatedTime = 0.0f;
		m_NumRaysProcessed = 0;
	}
}

void Renderer::UnpackToTexture()
{
	m_Texture->Bind();
	m_PixelBuffer->Bind();
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Texture->GetWidth(), m_Texture->GetHeight(), GL_RGBA, GL_UNSIGNED_BYTE, 0);
	m_PixelBuffer->Unbind();
}

void Renderer::OnResize(uint32_t width, uint32_t height)
{
	if ((m_ViewportWidth != width || m_ViewportHeight != height) && width != 0 && height != 0)
	{
		m_FrameNumber = 0;
		m_MRPS = 0;
		m_NumRaysProcessed = 0;
		m_Texture->OnResize(width, height);
		m_PixelBuffer->OnResize(width, height);
		checkCudaErrors(cudaFree((void*)m_AccumulationBuffer));
		checkCudaErrors(cudaMalloc((void**)&m_AccumulationBuffer, width * height * sizeof(float3)));

		m_ViewportWidth = width;
		m_ViewportHeight = height;
	}
}

void Renderer::SaveScreenshot()
{
	int width = m_Texture->GetWidth();
	int height = m_Texture->GetHeight();
	std::vector<unsigned char> pixels(width * height * 4);

	glBindTexture(GL_TEXTURE_2D, m_Texture->GetHandle());
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	stbi_flip_vertically_on_write(1);

	std::string filepath = FileDialog::SaveFile(
		"PNG image (*.png)\0*.png\0"
	);

	const std::string extension = ".png";

	if (!filepath.empty())
	{
		// Add extension if necessary
		if (filepath.length() < extension.length() ||
			filepath.compare(filepath.size() - extension.size(), extension.size(), extension) != 0)
			filepath += extension;

		if (!stbi_write_png(filepath.c_str(), width, height, 4, pixels.data(), width * 4))
		{
			std::cerr << "Failed to save screenshot to " << filepath << std::endl;
		}
	}

	std::cout << "Screenshot saved at: " << filepath.c_str() << std::endl;
}
