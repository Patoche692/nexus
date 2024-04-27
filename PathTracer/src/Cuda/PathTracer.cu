#include "PathTracer.cuh"
#include "Random.cuh"
#include "BRDF.cuh"
#include "BSDF.cuh"
#include "Utils/cuda_math.h"
#include "Utils/Utils.h"
#include "Camera.h"
#include "Geometry/BVH/TLAS.h"


__constant__ __device__ CameraData cameraData;
extern __constant__ __device__ Material* materials;
extern __constant__ __device__ Texture* textures;
extern __constant__ __device__ Mesh* bvhs;
extern __constant__ __device__ TLAS tlas;

inline __device__ uint32_t toColorUInt(float3 color)
{
	float4 clamped = clamp(make_float4(color, 1.0f), make_float4(0.0f), make_float4(1.0f));
	uint8_t red = (uint8_t)(clamped.x * 255.0f);
	uint8_t green = (uint8_t)(clamped.y * 255.0f);
	uint8_t blue = (uint8_t)(clamped.z * 255.0f);
	uint8_t alpha = (uint8_t)(clamped.w * 255.0f);
	 
	return alpha << 24 | blue << 16 | green << 8 | red;
}

inline __device__ float3 color(Ray& r, unsigned int& rngState)
{
	Ray currentRay = r;
	float3 currentAttenuation = make_float3(1.0f);

	for (int j = 0; j < 8; j++)
	{
		// Reset the hit position and calculate the inverse of the new direction
		currentRay.hit.t = 1e30f;
		currentRay.invDirection = 1 / currentRay.direction;

		tlas.Intersect(currentRay);

		// If no intersection, sample background
		if (currentRay.hit.t == 1e30f)
			return currentAttenuation * make_float3(0.2f);

		HitResult hitResult;
		hitResult.p = currentRay.origin + currentRay.direction * currentRay.hit.t;
		hitResult.rIn = currentRay;

		BVHInstance& instance = tlas.blas[currentRay.hit.instanceIdx];
		Triangle& triangle = instance.bvh->triangles[currentRay.hit.triIdx];
		float u = currentRay.hit.u, v = currentRay.hit.v;

		// Interpolating and rotating the normal
		hitResult.normal = u * triangle.normal1 + v * triangle.normal2 + (1 - (u + v)) * triangle.normal0;
		hitResult.normal = normalize(instance.transform.TransformVector(hitResult.normal));

		hitResult.material = materials[instance.materialId];

		if (hitResult.material.diffuseMapId == -1)
			hitResult.albedo = hitResult.material.diffuse;
		else
		{
			float2 uv = u * triangle.texCoord1 + v * triangle.texCoord2 + (1 - (u + v)) * triangle.texCoord0;
			hitResult.material.diffuse = textures[hitResult.material.diffuseMapId].GetPixel(uv.x, uv.y);
		}
		// Normal flipping
		//if (dot(hitResult.normal, currentRay.direction) > 0.0f)
		//	hitResult.normal = -hitResult.normal;

		float3 attenuation = make_float3(1.0f);
		float3 scatteredDir = make_float3(0.0f);

		BSDF bsdf;
		if (bsdf.Eval(hitResult, attenuation, scatteredDir, rngState))
		{
			currentAttenuation *= attenuation;
			currentRay.origin = hitResult.p;
			currentRay.direction = scatteredDir;
		}

		//switch (hitResult.material.type)
		//{
		//case Material::Type::DIFFUSE:
		//	if (hitResult.material.textureId == -1)
		//		hitResult.albedo = hitResult.material.diffuse.albedo;
		//	else
		//	{
		//		float2 uv = u * triangle.texCoord1 + v * triangle.texCoord2 + (1 - (u + v)) * triangle.texCoord0;
		//		hitResult.albedo = textures[hitResult.material.textureId].GetPixel(uv.x, uv.y);
		//	}

		//	if (diffuseScatter(hitResult, attenuation, currentRay, rngState))
		//	{
		//		currentAttenuation *= attenuation;
		//	}
		//	break;
		//case Material::Type::METAL:
		//	if (hitResult.material.textureId == -1)
		//		hitResult.albedo = hitResult.material.diffuse.albedo;
		//	else
		//	{
		//		float2 uv = u * triangle.texCoord1 + v * triangle.texCoord2 + (1 - (u + v)) * triangle.texCoord0;
		//		hitResult.albedo = textures[hitResult.material.textureId].GetPixel(uv.x, uv.y);
		//	}
		//	if (plasticScattter(hitResult, attenuation, currentRay, rngState))
		//	{
		//		currentAttenuation *= attenuation;
		//	}
		//	break;
		//case Material::Type::DIELECTRIC:
		//	if (dielectricScattter(hitResult, attenuation, currentRay, rngState))
		//	{
		//		currentAttenuation *= attenuation;
		//	}
		//	break;
		//case Material::Type::LIGHT:
		//	return currentAttenuation * hitResult.material.light.emission;
		//	break;
		//default:
		//	break;
		//}

		// Russian roulette
		float p = fmax(currentAttenuation.x, fmax(currentAttenuation.y, currentAttenuation.z));
		if (Random::Rand(rngState) > p)
			return make_float3(0.0f);

		// To get unbiased results, we need to increase the contribution of
		// the non-terminated rays with their probability to be terminated
		currentAttenuation *= 1.0f / p;
	}

	return make_float3(0.0f);
}

__global__ void traceRay(uint32_t* outBufferPtr, uint32_t frameNumber, float3* accumulationBuffer)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	int j = blockIdx.y * blockDim.y + threadIdx.y;

	uint2 pixel = make_uint2(i, j);

	uint2 resolution = cameraData.resolution;

	if (pixel.x >= resolution.x || pixel.y >= resolution.y)
		return;

	unsigned int rngState = Random::InitRNG(pixel, resolution, frameNumber);

	// Avoid using modulo, it significantly impacts performance
	float x = (pixel.x + Random::Rand(rngState)) / (float)resolution.x;
	float y = (pixel.y + Random::Rand(rngState)) / (float)resolution.y;

	float2 rd = cameraData.lensRadius * Random::RandomInUnitDisk(rngState);
	float3 offset = cameraData.right * rd.x + cameraData.up * rd.y;

	Ray ray(
		cameraData.position + offset,
		normalize(cameraData.lowerLeftCorner + x * cameraData.viewportX + y * cameraData.viewportY - cameraData.position - offset)
	);

	float3 c = color(ray, rngState);	// get new color
	if (frameNumber == 1)
		accumulationBuffer[pixel.y * resolution.x + pixel.x] = c;
	else
		accumulationBuffer[pixel.y * resolution.x + pixel.x] += c;

	c = accumulationBuffer[pixel.y * resolution.x + pixel.x] / frameNumber;

	// Gamma correction
	c = make_float3(pow(c.x, 0.455), pow(c.y, 0.455), pow(c.z, 0.455));

	outBufferPtr[pixel.y * resolution.x + pixel.x] = toColorUInt(c);	// convert color
}

void RenderViewport(std::shared_ptr<PixelBuffer> pixelBuffer, uint32_t frameNumber, float3* accumulationBuffer)
{
	checkCudaErrors(cudaGraphicsMapResources(1, &pixelBuffer->GetCudaResource()));
	size_t size = 0;
	uint32_t* devicePtr = 0;
	checkCudaErrors(cudaGraphicsResourceGetMappedPointer((void**)&devicePtr, &size, pixelBuffer->GetCudaResource()));

	uint32_t tx = 16, ty = 16;
	dim3 blocks(pixelBuffer->GetWidth() / tx + 1, pixelBuffer->GetHeight() / ty + 1);
	dim3 threads(tx, ty);

	traceRay<<<blocks, threads>>>(devicePtr, frameNumber, accumulationBuffer);

	checkCudaErrors(cudaGetLastError());
	checkCudaErrors(cudaGraphicsUnmapResources(1, &pixelBuffer->GetCudaResource(), 0));
}

void SendCameraDataToDevice(Camera* camera)
{
	float3 position = camera->GetPosition();
	float3 forwardDirection = camera->GetForwardDirection();
	float3 rightDirection = camera->GetRightDirection();
	float3 upDirection = cross(rightDirection, forwardDirection);

	float aspectRatio = camera->GetViewportWidth() / (float)camera->GetViewportHeight();
	float halfHeight = camera->GetFocusDist() * tanf(camera->GetVerticalFOV() / 2.0f * M_PI / 180.0f);
	float halfWidth = aspectRatio * halfHeight;

	float3 viewportX = 2 * halfWidth * rightDirection;
	float3 viewportY = 2 * halfHeight * upDirection;
	float3 lowerLeftCorner = position - viewportX / 2.0f - viewportY / 2.0f + forwardDirection * camera->GetFocusDist();

	float lensRadius = camera->GetFocusDist() * tanf(camera->GetDefocusAngle() / 2.0f * M_PI / 180.0f);

	CameraData data = {
		position,
		rightDirection,
		upDirection,
		lensRadius,
		lowerLeftCorner,
		viewportX,
		viewportY,
		make_uint2(camera->GetViewportWidth(), camera->GetViewportHeight())
	};
	checkCudaErrors(cudaMemcpyToSymbol(cameraData, &data, sizeof(CameraData)));
}