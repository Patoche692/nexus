#pragma once

#include <device_launch_parameters.h>
#include <iostream>
#include <glm/glm.hpp>
#include "OpenGL/PixelBuffer.h"
#include "Scene.h"
#include "AssetManager.cuh"

// Number of threads in a block
#define BLOCK_SIZE 8

#define WARP_SIZE 32	// Same size for all NVIDIA GPUs


struct CameraData
{
	float3 position;
	float3 right;
	float3 up;
	float lensRadius;
	float3 lowerLeftCorner;
	float3 viewportX;
	float3 viewportY;
	uint2 resolution;
};

struct SceneData
{
	bool hasHdrMap;
	cudaTextureObject_t hdrMap;
};

void RenderViewport(std::shared_ptr<PixelBuffer> pixelBuffer, uint32_t frameNumber, float3* accumulationBuffer);
void InitDeviceSceneData();
void SendHDRMapToDevice(const Texture& map);
void SendCameraDataToDevice(Camera *camera);
