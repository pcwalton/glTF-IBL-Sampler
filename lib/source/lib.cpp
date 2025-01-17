#include "GltfIblSampler.h"
#include "vkHelper.h"
#include "STBImage.h"
#include "FileHelper.h"
#include "ktxImage.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <cstring>
#include <cassert>

#include "format.h"

namespace IBLLib
{

const uint32_t panoramaToCubeMapShaderSource[] = {
#include "shaders/gen/panorama_to_cube_map.frag.inc"
};

const uint32_t filterCubeMapShaderSource[] = {
#include "shaders/gen/filter_cube_map.frag.inc"
};

const uint32_t primitiveShaderSource[] = {
#include "shaders/gen/primitive.vert.inc"
};

Result compileShader(vkHelper& _vulkan, VkShaderModule& _outModule, const uint32_t *_spvData, size_t _spvWordLength)
{
	std::vector<uint32_t> outSpvBlob(&_spvData[0], &_spvData[_spvWordLength]);

	if (_vulkan.loadShaderModule(_outModule, outSpvBlob.data(), outSpvBlob.size() * 4) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	return Result::Success;
}

Result uploadImage(vkHelper& _vulkan, int width, int height, int faces, const float *hdrData, uint32_t &_defaultCubemapResolution, uint32_t explicitCubemapResolution, uint32_t explicitMipCount, VkImage& _outImage)
{
	if (faces == 6)
	{
		_defaultCubemapResolution = height;
	}
	else
	{
		// it is best to sample an nxn cube map from a 4nx2n equirectangular image, e.g. a 1024x512 equirectangular images becomes a 256x256 cube map.
		_defaultCubemapResolution = height / 2;
	}

	uint32_t cubemapResolution;
	if (explicitCubemapResolution == 0)
	{
		cubemapResolution = _defaultCubemapResolution;
	}
	else
	{
		cubemapResolution = explicitCubemapResolution;
	}

	uint32_t maxMipLevels = 0;
	if (explicitMipCount == 0)
	{
		for (uint32_t m = cubemapResolution; m > 0; m = m >> 1, ++maxMipLevels) {}
	}
	else
	{
		maxMipLevels = explicitMipCount;
	}

	VkCommandBuffer uploadCmds = VK_NULL_HANDLE;
	if (_vulkan.createCommandBuffer(uploadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	// Calculate mip size.
	uint32_t byteSize = 0;
	uint32_t mipWidth = static_cast<uint32_t>(width), mipHeight = static_cast<uint32_t>(height);
	for (uint32_t mip = 0; mip < maxMipLevels; mip++) {
		byteSize += mipWidth * mipHeight * static_cast<uint32_t>(faces) * 4 * sizeof(float);
		mipWidth /= 2;
		mipHeight /= 2;
	}

	// create staging buffer for image data
	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	if (_vulkan.createBufferAndAllocate(stagingBuffer, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	// transfer data to the host coherent staging buffer
	uint32_t firstMipByteSize = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * static_cast<uint32_t>(faces) * 4 * sizeof(float);
	if (_vulkan.writeBufferData(stagingBuffer, hdrData, firstMipByteSize) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	// create the destination image we want to sample in the shader
	if (_vulkan.createImage2DAndAllocate(
		_outImage,
		width,
		height,
		VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		faces == 6 ? maxMipLevels : 1,
		faces,
		VK_IMAGE_TILING_OPTIMAL,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		VK_SHARING_MODE_EXCLUSIVE,
		faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (_vulkan.beginCommandBuffer(uploadCmds, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	// transition to write dst layout
	_vulkan.transitionImageToTransferWrite(uploadCmds, _outImage);
	_vulkan.copyBufferToBasicImage2D(uploadCmds, stagingBuffer, _outImage);
	_vulkan.transitionImageToShaderRead(uploadCmds, _outImage);

	if (_vulkan.endCommandBuffer(uploadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (_vulkan.executeCommandBuffer(uploadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	_vulkan.destroyBuffer(stagingBuffer);
	_vulkan.destroyCommandBuffer(uploadCmds);

	return Result::Success;
}

Result uploadImage(vkHelper& _vulkan, const char* _inputPath, VkImage& _outImage, uint32_t& _defaultCubemapResolution, uint32_t explicitCubemapResolution, uint32_t explicitMipCount, bool& _isCubemap)
{
	_outImage = VK_NULL_HANDLE;

	{
		std::ifstream inputFile(_inputPath, std::ios::binary);

		KTXHeader ktxHeader;
		inputFile.read(reinterpret_cast<char *>(&ktxHeader), sizeof(ktxHeader));
		if (inputFile.gcount() == sizeof(ktxHeader) && memcmp(ktxHeader.identifier, KTX_IDENTIFIER, sizeof(KTX_IDENTIFIER)) == 0)
		{
			// This is a KTX2 file.
			inputFile.seekg(sizeof(KTXIndex), std::ios_base::cur);
			KTXLevelIndex ktxLevelIndex;
			inputFile.read(reinterpret_cast<char *>(&ktxLevelIndex), sizeof(ktxLevelIndex));
			inputFile.seekg(ktxLevelIndex.byteOffset, std::ios_base::beg);

			std::vector<float> cubemapData;
			cubemapData.resize(ktxLevelIndex.byteLength / sizeof(float));
			inputFile.read(reinterpret_cast<char *>(&cubemapData[0]), ktxLevelIndex.byteLength);
			if (static_cast<uint64_t>(inputFile.gcount()) != ktxLevelIndex.byteLength)
			{
				return Result::InputPanoramaFileNotFound;
			}

			_isCubemap = true;
			return uploadImage(_vulkan, ktxHeader.pixelWidth, ktxHeader.pixelHeight, ktxHeader.faceCount, &cubemapData[0], _defaultCubemapResolution, explicitCubemapResolution, explicitMipCount, _outImage);
		}
	}

	STBImage panorama;

	if (panorama.loadHdr(_inputPath) != Result::Success)
	{
		return Result::InputPanoramaFileNotFound;
	}

	_isCubemap = false;
	return uploadImage(_vulkan, panorama.getWidth(), panorama.getHeight(), 1, panorama.getHdrData(), _defaultCubemapResolution, explicitCubemapResolution, explicitMipCount, _outImage);
}

Result convertVkFormat(vkHelper& _vulkan, const VkCommandBuffer _commandBuffer, const VkImage _srcImage, VkImage& _outImage, VkFormat _dstFormat, const VkImageLayout inputImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
{
	const VkImageCreateInfo* pInfo = _vulkan.getCreateInfo(_srcImage);

	if (pInfo == nullptr)
	{
		return Result::InvalidArgument;
	}

	if (_outImage != VK_NULL_HANDLE)
	{
		printf("Expecting empty outImage\n");
		return Result::InvalidArgument;
	}

	const VkFormat srcFormat = pInfo->format;
	const uint32_t sideLength = pInfo->extent.width;
	const uint32_t mipLevels = pInfo->mipLevels;
	const uint32_t arrayLayers = pInfo->arrayLayers;

	if (_vulkan.createImage2DAndAllocate(_outImage, sideLength, sideLength, _dstFormat,
																			 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
																			 mipLevels, arrayLayers, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	VkImageSubresourceRange subresourceRange{};
	subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresourceRange.baseMipLevel = 0;
	subresourceRange.levelCount = mipLevels;
	subresourceRange.layerCount = arrayLayers;
	
	_vulkan.imageBarrier(_commandBuffer, _outImage,
											 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,//src stage, access
											 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,//dst stage, access
											 subresourceRange);

	_vulkan.imageBarrier(_commandBuffer, _srcImage,
											 inputImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
											 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,//src stage, access
											 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,//dst stage, access
											 subresourceRange);
	
	uint32_t currentSideLength = sideLength;

	for (uint32_t level = 0; level < mipLevels; level++)
	{
		VkImageBlit imageBlit{};

		// Source
		imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageBlit.srcSubresource.layerCount = arrayLayers;
		imageBlit.srcSubresource.mipLevel = level;
		imageBlit.srcOffsets[1].x = currentSideLength;
		imageBlit.srcOffsets[1].y = currentSideLength;
		imageBlit.srcOffsets[1].z = 1;

		// Destination
		imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageBlit.dstSubresource.layerCount = arrayLayers;
		imageBlit.dstSubresource.mipLevel = level;
		imageBlit.dstOffsets[1].x = currentSideLength;
		imageBlit.dstOffsets[1].y = currentSideLength;
		imageBlit.dstOffsets[1].z = 1;

		vkCmdBlitImage(
									 _commandBuffer,
									 _srcImage,
									 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
									 _outImage,
									 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									 1,
									 &imageBlit,
									 VK_FILTER_LINEAR);

		currentSideLength = currentSideLength >> 1;
	}

	return Result::Success;
}

void convertImageOnCPU(std::vector<uint8_t> &destBuffer, const std::vector<uint8_t> &srcBuffer, VkFormat destFormat, VkFormat srcFormat)
{
	if (destFormat == srcFormat) {
		std::copy(srcBuffer.begin(), srcBuffer.end(), std::back_inserter(destBuffer));
		return;
	}

	assert(destFormat == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && srcFormat == VK_FORMAT_R32G32B32A32_SFLOAT);

	for (uint32_t i = 0; i < srcBuffer.size() / 16; i++) {
		float r = *(float *)&srcBuffer[i * 16 + 0];
		float g = *(float *)&srcBuffer[i * 16 + 4];
		float b = *(float *)&srcBuffer[i * 16 + 8];
		float a = *(float *)&srcBuffer[i * 16 + 12];

		// https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_shared_exponent.txt
		const float N = 9.0f, B = 15.0f;
		const float sharedexpMax = 65408.0f;
		float redC = fmaxf(0.0f, fminf(sharedexpMax, r));
		float greenC = fmaxf(0.0f, fminf(sharedexpMax, g));
		float blueC = fmaxf(0.0f, fminf(sharedexpMax, b));

		float maxC = fmaxf(fmaxf(redC, greenC), blueC);

		float expSharedP = fmaxf(-16.0f, floorf(log2f(maxC))) + 16.0f;

		float maxS = floorf(maxC / powf(2.0f, expSharedP - B - N) + 0.5f);
		float expShared = maxS < 512.0f ? expSharedP : expSharedP + 1.0f;

		float redS = floorf(redC / powf(2.0f, expShared - B - N) + 0.5f);
		float greenS = floorf(greenC / powf(2.0f, expShared - B - N) + 0.5f);
		float blueS = floorf(blueC / powf(2.0f, expShared - B - N) + 0.5f);

		uint32_t packedR = static_cast<uint32_t>(redS);
		uint32_t packedG = static_cast<uint32_t>(greenS);
		uint32_t packedB = static_cast<uint32_t>(blueS);
		uint32_t packedE = static_cast<uint32_t>(expShared);

		uint32_t packed = packedR | (packedG << 9) | (packedB << 18) | (packedE << 27);
		std::copy((uint8_t *)&packed, (uint8_t *)(&packed + 1), std::back_inserter(destBuffer));
	}
}

Result downloadCubemap(vkHelper& _vulkan, const VkImage _srcImage, const char* _outputPath, const VkFormat targetFormat, const VkImageLayout inputImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
{
	const VkImageCreateInfo* pInfo = _vulkan.getCreateInfo(_srcImage);
	if (pInfo == nullptr)
	{
		return Result::InvalidArgument;
	}

	Result res = Success;

	const VkFormat cubeMapFormat = pInfo->format;
	const uint32_t cubeMapFormatByteSize = getFormatSize(cubeMapFormat);
	const uint32_t targetFormatByteSize = getFormatSize(targetFormat);
	const uint32_t cubeMapSideLength = pInfo->extent.width;
	const uint32_t mipLevels = pInfo->mipLevels;

	using Faces = std::vector<VkBuffer>;
	using MipLevels = std::vector<Faces>;

	MipLevels stagingBuffer(mipLevels);

	{
		uint32_t currentSideLength = cubeMapSideLength;

		for (uint32_t level = 0; level < mipLevels; level++)
		{
			Faces& faces = stagingBuffer[level];
			faces.resize(6u);

			for (uint32_t face = 0; face < 6u; face++)
			{
				if (_vulkan.createBufferAndAllocate(
																						faces[face], currentSideLength * currentSideLength * cubeMapFormatByteSize,
																						VK_BUFFER_USAGE_TRANSFER_DST_BIT,// VkBufferUsageFlags _usage,
																						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)//VkMemoryPropertyFlags _memoryFlags,
						!= VK_SUCCESS)
				{
					return Result::VulkanError;
				}
			}

			currentSideLength = currentSideLength >> 1;
		}
	}

	VkCommandBuffer downloadCmds = VK_NULL_HANDLE;
	if (_vulkan.createCommandBuffer(downloadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (_vulkan.beginCommandBuffer(downloadCmds, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	// barrier on complete image
	VkImageSubresourceRange  subresourceRange{};
	subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresourceRange.baseArrayLayer = 0u;
	subresourceRange.layerCount = 6u;
	subresourceRange.baseMipLevel = 0u;
	subresourceRange.levelCount = mipLevels;

	_vulkan.imageBarrier(downloadCmds, _srcImage,
											 inputImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
											 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // src stage, access
											 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
											 subresourceRange);//dst stage, access

	// copy all faces & levels into staging buffers
	{
		uint32_t currentSideLength = cubeMapSideLength;

		VkBufferImageCopy region{};

		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1u;

		for (uint32_t level = 0; level < mipLevels; level++)
		{
			region.imageSubresource.mipLevel = level;
			Faces& faces = stagingBuffer[level];

			for (uint32_t face = 0; face < 6u; face++)
			{
				region.imageSubresource.baseArrayLayer = face;
				region.imageExtent = { currentSideLength , currentSideLength , 1u };

				_vulkan.copyImage2DToBuffer(downloadCmds, _srcImage, faces[face], region);
			}

			currentSideLength = currentSideLength >> 1;
		}
	}

	if (_vulkan.endCommandBuffer(downloadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (_vulkan.executeCommandBuffer(downloadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	_vulkan.destroyCommandBuffer(downloadCmds);

	// Image is copied to buffer
	// Now map buffer and copy to ram
	{
		KtxImage ktxImage(cubeMapSideLength, cubeMapSideLength, targetFormat, mipLevels, true);

		std::vector<uint8_t> cubemapImageData, targetImageData;

		uint32_t currentSideLength = cubeMapSideLength;

		for (uint32_t level = 0; level < mipLevels; level++)
		{
			const size_t cubemapByteSize = (size_t)currentSideLength * (size_t)currentSideLength * (size_t)cubeMapFormatByteSize;
			const size_t targetByteSize = (size_t)currentSideLength * (size_t)currentSideLength * (size_t)targetFormatByteSize;
			cubemapImageData.resize(cubemapByteSize);

			Faces& faces = stagingBuffer[level];

			for (uint32_t face = 0; face < 6u; face++)
			{
				if (_vulkan.readBufferData(faces[face], cubemapImageData.data(), cubemapByteSize) != VK_SUCCESS)
				{
					return Result::VulkanError;
				}

				targetImageData.clear();
				convertImageOnCPU(targetImageData, cubemapImageData, targetFormat, cubeMapFormat);

				res = ktxImage.writeFace(targetImageData, face, level);

				if (res != Result::Success)
				{
					return res;
				}

				_vulkan.destroyBuffer(faces[face]);
			}

			currentSideLength = currentSideLength >> 1;
		}

		res = ktxImage.save(_outputPath);
		if (res != Result::Success)
		{
			printf("Could not save to path %s \n", _outputPath);
			return res;
		}
	}

	return Result::Success;
}

Result download2DImage(vkHelper& _vulkan, const VkImage _srcImage, const char* _outputPath, const VkImageLayout inputImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
{
	const VkImageCreateInfo* pInfo = _vulkan.getCreateInfo(_srcImage);
	if (pInfo == nullptr)
	{
		return Result::InvalidArgument;
	}

	Result res = Success;

	const VkFormat format = pInfo->format;
	const uint32_t formatByteSize = getFormatSize(format);
	const uint32_t width = pInfo->extent.width;
	const uint32_t height = pInfo->extent.width;
	const size_t imageByteSize = width * height * formatByteSize;

	VkBuffer stagingBuffer{};

	if (_vulkan.createBufferAndAllocate(
																			stagingBuffer, static_cast<uint32_t>(imageByteSize),
																			VK_BUFFER_USAGE_TRANSFER_DST_BIT,// VkBufferUsageFlags _usage,
																			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)//VkMemoryPropertyFlags _memoryFlags,
			!= VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	VkCommandBuffer downloadCmds = VK_NULL_HANDLE;
	if (_vulkan.createCommandBuffer(downloadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (_vulkan.beginCommandBuffer(downloadCmds, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	// barrier on complete image
	VkImageSubresourceRange  subresourceRange{};
	subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresourceRange.baseArrayLayer = 0u;
	subresourceRange.layerCount = 1u;
	subresourceRange.baseMipLevel = 0u;
	subresourceRange.levelCount = 1u;

	_vulkan.imageBarrier(downloadCmds, _srcImage,
											 inputImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
											 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // src stage, access
											 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
											 subresourceRange);//dst stage, access

	// copy 2D image to buffer
	{
		VkBufferImageCopy region{};

		region.imageExtent = pInfo->extent;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1u;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;

		_vulkan.copyImage2DToBuffer(downloadCmds, _srcImage, stagingBuffer, region);
	}

	if (_vulkan.endCommandBuffer(downloadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (_vulkan.executeCommandBuffer(downloadCmds) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	_vulkan.destroyCommandBuffer(downloadCmds);

	// Image is copied to buffer
	// Now map buffer and copy to ram
	{

		std::vector<uint8_t> imageData;
		imageData.resize(imageByteSize);

		if (_vulkan.readBufferData(stagingBuffer, imageData.data(), imageByteSize) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		// Compute channel count by dividing the pixel byte length through each channels byte length.
		const uint32_t channels = getChannelCount(pInfo->format);

		// Copy the outputted image (format with 1, 2 or 4 channels) into a 3-channel image.
		// This is kind of a hack (this function is currently only used to write the BRDF LUT to disk):
		// It seems that stb_write_image is not able to write PNGs with 4 components,
		// and 2-channel images are displayed as grey-alpha,
		// which makes is impossible to compare the outputted LUT with already
		// existing LUT PNGs.
		std::vector<uint8_t> imageDataThreeChannel(imageData.size() * (4 / channels), 0);
		for (uint32_t x = 0; x < width; x++) {
			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t c = 0; c < std::min(channels, 3u); c++) {
					imageDataThreeChannel[3 * (x * width + y) + c] =
					imageData[channels * (x * width + y) + c];
				}
			}
		}

		STBImage stb_image;
		res = stb_image.savePng(_outputPath, width, height, 3, imageDataThreeChannel.data());
		if (res != Result::Success)
		{
			return res;
		}

		_vulkan.destroyBuffer(stagingBuffer);
	}

	return Result::Success;
}

void generateMipmapLevels(vkHelper& _vulkan, const VkCommandBuffer _commandBuffer, const VkImage _image, uint32_t _maxMipLevels, uint32_t _sideLength, const VkImageLayout _currentImageLayout)
{
	{
		VkImageSubresourceRange mipbaseRange{};
		mipbaseRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		mipbaseRange.baseMipLevel = 0u;
		mipbaseRange.levelCount = 1u;
		mipbaseRange.layerCount = 6u;

		_vulkan.imageBarrier(_commandBuffer, _image,
												 _currentImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
												 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
												 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,//dst stage, access
												 mipbaseRange);
	}

	for (uint32_t i = 1; i < _maxMipLevels; i++)
	{
		VkImageBlit imageBlit{};

		// Source
		imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageBlit.srcSubresource.layerCount = 6u;
		imageBlit.srcSubresource.mipLevel = i - 1;
		imageBlit.srcOffsets[1].x = int32_t(_sideLength >> (i - 1));
		imageBlit.srcOffsets[1].y = int32_t(_sideLength >> (i - 1));
		imageBlit.srcOffsets[1].z = 1;

		// Destination
		imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageBlit.dstSubresource.layerCount = 6u;
		imageBlit.dstSubresource.mipLevel = i;
		imageBlit.dstOffsets[1].x = int32_t(_sideLength >> i);
		imageBlit.dstOffsets[1].y = int32_t(_sideLength >> i);
		imageBlit.dstOffsets[1].z = 1;

		VkImageSubresourceRange mipSubRange = {};
		mipSubRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		mipSubRange.baseMipLevel = i;
		mipSubRange.levelCount = 1;
		mipSubRange.layerCount = 6u;

		//  Transiton current mip level to transfer dest
		_vulkan.imageBarrier(_commandBuffer, _image,
												 _currentImageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
												 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
												 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
												 mipSubRange);//dst stage, access

		vkCmdBlitImage(
									 _commandBuffer,
									 _image,
									 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
									 _image,
									 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									 1,
									 &imageBlit,
									 VK_FILTER_LINEAR);


		//  Transiton  back
		_vulkan.imageBarrier(_commandBuffer, _image,
												 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
												 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
												 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,//dst stage, access
												 mipSubRange);

	}

	{
		VkImageSubresourceRange completeRange{};
		completeRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		completeRange.baseMipLevel = 0;
		completeRange.levelCount = _maxMipLevels;
		completeRange.layerCount = 6u;

		_vulkan.imageBarrier(_commandBuffer, _image,
												 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
												 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
												 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,//dst stage, access
												 completeRange);
	}
}

Result panoramaToCubemap(vkHelper& _vulkan, const VkCommandBuffer _commandBuffer, /*const VkRenderPass _renderPass,*/ const VkShaderModule fullscreenVertexShader, const VkImage _panoramaImage, const VkImage _cubeMapImage)
{
	IBLLib::Result res = Result::Success;

	const VkImageCreateInfo* textureInfo = _vulkan.getCreateInfo(_cubeMapImage);

	if (textureInfo == nullptr)
	{
		return Result::InvalidArgument;
	}

	const uint32_t cubeMapSideLength = textureInfo->extent.width;
	const uint32_t maxMipLevels = textureInfo->mipLevels;
	const VkFormat format = textureInfo->format;

	VkShaderModule panoramaToCubeMapFragmentShader = VK_NULL_HANDLE;
	if ((res = compileShader(
		_vulkan,
		panoramaToCubeMapFragmentShader,
		panoramaToCubeMapShaderSource,
		sizeof(panoramaToCubeMapShaderSource) / sizeof(panoramaToCubeMapShaderSource[0]))) !=
		Result::Success)
	{
		return res;
	}

	VkRenderPass renderPass = VK_NULL_HANDLE;
	{
		RenderPassDesc renderPassDesc;

		// add rendertargets (cubemap faces)
		for (int face = 0; face < 6; ++face)
		{
			renderPassDesc.addAttachment(format);
		}
		if (_vulkan.createRenderPass(renderPass, renderPassDesc.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkSamplerCreateInfo samplerInfo{};
	_vulkan.fillSamplerCreateInfo(samplerInfo);

	samplerInfo.maxLod = float(maxMipLevels + 1);
	VkSampler panoramaSampler = VK_NULL_HANDLE;
	if (_vulkan.createSampler(panoramaSampler, samplerInfo) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	VkImageView panoramaImageView = VK_NULL_HANDLE;
	if (_vulkan.createImageView(panoramaImageView, _panoramaImage) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	VkPipelineLayout panoramaPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSet panoramaSet = VK_NULL_HANDLE;
	VkPipeline panoramaToCubeMapPipeline = VK_NULL_HANDLE;
	{
		//
		// Create pipeline layout
		//
		VkDescriptorSetLayout panoramaSetLayout = VK_NULL_HANDLE;
		DescriptorSetInfo setLayout0;
		setLayout0.addCombinedImageSampler(panoramaSampler, panoramaImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		if (setLayout0.create(_vulkan, panoramaSetLayout, panoramaSet) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		_vulkan.updateDescriptorSets(setLayout0.getWrites());

		if (_vulkan.createPipelineLayout(panoramaPipelineLayout, panoramaSetLayout) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		GraphicsPipelineDesc panoramaToCubePipeline;

		panoramaToCubePipeline.addShaderStage(fullscreenVertexShader, VK_SHADER_STAGE_VERTEX_BIT, "main");
		panoramaToCubePipeline.addShaderStage(panoramaToCubeMapFragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT, "main");

		panoramaToCubePipeline.setRenderPass(renderPass);
		panoramaToCubePipeline.setPipelineLayout(panoramaPipelineLayout);

		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_FALSE;

		panoramaToCubePipeline.addColorBlendAttachment(colorBlendAttachment,6);

		panoramaToCubePipeline.setViewportExtent(VkExtent2D{ cubeMapSideLength, cubeMapSideLength });

		if (_vulkan.createPipeline(panoramaToCubeMapPipeline, panoramaToCubePipeline.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	/// Render Pass
	std::vector<VkImageView> inputCubeMapViews(6u, VK_NULL_HANDLE);
	for (size_t i = 0; i < inputCubeMapViews.size(); i++)
	{
		if (_vulkan.createImageView(inputCubeMapViews[i], _cubeMapImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, static_cast<uint32_t>(i), 1u }) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkFramebuffer cubeMapInputFramebuffer = VK_NULL_HANDLE;
	if (_vulkan.createFramebuffer(cubeMapInputFramebuffer, renderPass, cubeMapSideLength, cubeMapSideLength, inputCubeMapViews, 1u) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	{
		VkImageSubresourceRange  subresourceRangeBaseMiplevel = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, maxMipLevels, 0u, 6u };

		_vulkan.imageBarrier(_commandBuffer, _cubeMapImage,
												 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
												 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,// src stage, access
												 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, //dst stage, access
												 subresourceRangeBaseMiplevel);
	}

	_vulkan.bindDescriptorSet(_commandBuffer, panoramaPipelineLayout, panoramaSet);

	vkCmdBindPipeline(_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, panoramaToCubeMapPipeline);

	const std::vector<VkClearValue> clearValues(6u, { 0.0f, 0.0f, 1.0f, 1.0f });

	_vulkan.beginRenderPass(_commandBuffer, renderPass, cubeMapInputFramebuffer, VkRect2D{ 0u, 0u, cubeMapSideLength, cubeMapSideLength }, clearValues);
	vkCmdDraw(_commandBuffer, 3, 1u, 0, 0);
	_vulkan.endRenderPass(_commandBuffer);

	return res;
}
} // !IBLLib


IBLLib::Result IBLLib::sample(const char* _inputPath, const char* _outputPathCubeMap, const char* _outputPathLUT, Distribution _distribution, unsigned int _cubemapResolution, unsigned int _mipmapCount, unsigned int _sampleCount, OutputFormat _targetFormat, float _lodBias, bool _debugOutput)
{
	const VkFormat cubeMapFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	const VkFormat LUTFormat = VK_FORMAT_R8G8B8A8_UNORM;

	IBLLib::Result res = Result::Success;

	vkHelper vulkan;

	if (vulkan.initialize(0u, 1u, _debugOutput) != VK_SUCCESS)
	{
		return Result::VulkanInitializationFailed;
	}

	VkImage panoramaImage;
	bool inputIsCubemap;

	uint32_t defaultCubemapResolution = 0;
	if ((res = uploadImage(vulkan, _inputPath, panoramaImage, defaultCubemapResolution, _cubemapResolution, _mipmapCount, inputIsCubemap)) != Result::Success)
	{
		return res;
	}

	if (_cubemapResolution == 0)
	{
		_cubemapResolution = defaultCubemapResolution;
	}

	VkShaderModule fullscreenVertexShader = VK_NULL_HANDLE;
	if ((res = compileShader(
		vulkan,
		fullscreenVertexShader,
		primitiveShaderSource,
		sizeof(primitiveShaderSource) / sizeof(primitiveShaderSource[0]))) !=
		Result::Success)
	{
		return res;
	}

	VkShaderModule filterCubeMapFragmentShader = VK_NULL_HANDLE;
	if ((res = compileShader(
		vulkan,
		filterCubeMapFragmentShader,
		filterCubeMapShaderSource,
		sizeof(filterCubeMapShaderSource) / sizeof(filterCubeMapShaderSource[0]))) !=
		Result::Success)
	{
		return res;
	}

	VkExtent3D panoramaExtent = vulkan.getCreateInfo(panoramaImage)->extent;
	_mipmapCount = _mipmapCount != 0 ? _mipmapCount : static_cast<uint32_t>(floor(log2(_cubemapResolution)));

	const uint32_t cubeMapSideLength = _cubemapResolution;
	const uint32_t maxMipLevels = _distribution == Distribution::Lambertian ? 1u : _mipmapCount;

	if ((_cubemapResolution >> (maxMipLevels - 1)) < 1)
	{
		printf("Error: CubemapResolution incompatible with MipmapCount\n");
		return Result::InvalidArgument;
	}
	
	VkSampler cubeMipMapSampler = VK_NULL_HANDLE;
	{
		VkSamplerCreateInfo samplerInfo{};
		vulkan.fillSamplerCreateInfo(samplerInfo);
		samplerInfo.maxLod = float(maxMipLevels + 1);

		if (vulkan.createSampler(cubeMipMapSampler, samplerInfo) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}
	
	VkImage inputCubeMap = VK_NULL_HANDLE;
	VkImageLayout currentInputCubeMapLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	//VK_IMAGE_USAGE_TRANSFER_SRC_BIT needed for transfer to staging buffer
	if (!inputIsCubemap)
	{
		if (vulkan.createImage2DAndAllocate(inputCubeMap, cubeMapSideLength, cubeMapSideLength, cubeMapFormat,
																				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
																				maxMipLevels, 6u, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}
	else
	{
		inputCubeMap = panoramaImage;
	}

	VkImageView inputCubeMapCompleteView = VK_NULL_HANDLE;
	if (vulkan.createImageView(inputCubeMapCompleteView, inputCubeMap, { VK_IMAGE_ASPECT_COLOR_BIT, 0u, maxMipLevels, 0u, 6u }, VK_FORMAT_UNDEFINED, VK_IMAGE_VIEW_TYPE_CUBE) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	VkImage outputCubeMap = VK_NULL_HANDLE;
	if (_distribution == IBLLib::Distribution::None)
	{
		outputCubeMap = inputCubeMap;
	}
	else if (vulkan.createImage2DAndAllocate(outputCubeMap, cubeMapSideLength, cubeMapSideLength, cubeMapFormat,
																					 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
																					 maxMipLevels, 6u, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	std::vector< std::vector<VkImageView> > outputCubeMapViews(maxMipLevels);
	for (uint32_t i = 0; i < maxMipLevels; ++i)
	{
		outputCubeMapViews[i].resize(6, VK_NULL_HANDLE); //sides of the cube

		for (uint32_t j = 0; j < 6; j++)
		{
			VkImageSubresourceRange subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
			subresourceRange.baseMipLevel = i;
			subresourceRange.baseArrayLayer = j;
			if (vulkan.createImageView(outputCubeMapViews[i][j], outputCubeMap, subresourceRange) != VK_SUCCESS)
			{
				return Result::VulkanError;
			}
		}
	}

	VkImageView outputCubeMapCompleteView = VK_NULL_HANDLE;
	{
		VkImageSubresourceRange subresourceRange{};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseArrayLayer = 0u;
		subresourceRange.baseMipLevel = 0u;
		subresourceRange.layerCount = 6u;
		subresourceRange.levelCount = maxMipLevels;

		if (vulkan.createImageView(outputCubeMapCompleteView, outputCubeMap, subresourceRange, VK_FORMAT_UNDEFINED, VK_IMAGE_VIEW_TYPE_CUBE) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkImage outputLUT = VK_NULL_HANDLE;
	VkImageView outputLUTView = VK_NULL_HANDLE;
	if (_distribution != IBLLib::Distribution::None)
	{
		if (vulkan.createImage2DAndAllocate(outputLUT, cubeMapSideLength, cubeMapSideLength, LUTFormat,
																				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT /*| VK_IMAGE_USAGE_SAMPLED_BIT*/,
																				1u, 1u, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		{
			VkImageSubresourceRange subresourceRange{};
			subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			subresourceRange.layerCount = 1u;
			subresourceRange.levelCount = 1u;

			if (vulkan.createImageView(outputLUTView, outputLUT, subresourceRange, VK_FORMAT_UNDEFINED, VK_IMAGE_VIEW_TYPE_2D) != VK_SUCCESS)
			{
				return Result::VulkanError;
			}
		}
	}

	VkRenderPass renderPass = VK_NULL_HANDLE;
	{
		RenderPassDesc renderPassDesc;

		// add rendertargets (cubemap faces)
		for (int face = 0; face < 6; ++face)
		{
			renderPassDesc.addAttachment(cubeMapFormat);
		}

		renderPassDesc.addAttachment(LUTFormat);		

		if (vulkan.createRenderPass(renderPass, renderPassDesc.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	//Push Constants for specular and diffuse filter passes
	struct PushConstant
	{
		float roughness = 0.f;
		uint32_t sampleCount = 1u;
		uint32_t mipLevel = 1u;
		uint32_t width = 1024u;
		float lodBias = 0.f;
		Distribution distribution = Distribution::Lambertian;
	};

	std::vector<VkPushConstantRange> ranges(1u);
	VkPushConstantRange& range = ranges.front();

	range.offset = 0u;
	range.size = sizeof(PushConstant);
	range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	////////////////////////////////////////////////////////////////////////////////////////
	// Filter CubeMap Pipeline
	VkDescriptorSet filterDescriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout filterPipelineLayout = VK_NULL_HANDLE;
	VkPipeline filterPipeline = VK_NULL_HANDLE;
	if (_distribution != IBLLib::Distribution::None)
	{
		DescriptorSetInfo setLayout0;
		uint32_t binding = 1u;
		setLayout0.addCombinedImageSampler(cubeMipMapSampler, inputCubeMapCompleteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, binding, VK_SHADER_STAGE_FRAGMENT_BIT); // change sampler ?

		VkDescriptorSetLayout filterSetLayout = VK_NULL_HANDLE;
		if (setLayout0.create(vulkan, filterSetLayout, filterDescriptorSet) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		vulkan.updateDescriptorSets(setLayout0.getWrites());

		if (vulkan.createPipelineLayout(filterPipelineLayout, filterSetLayout, ranges) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		GraphicsPipelineDesc filterCubeMapPipelineDesc;

		filterCubeMapPipelineDesc.addShaderStage(fullscreenVertexShader, VK_SHADER_STAGE_VERTEX_BIT, "main");
		filterCubeMapPipelineDesc.addShaderStage(filterCubeMapFragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT, "main");

		filterCubeMapPipelineDesc.setRenderPass(renderPass);
		filterCubeMapPipelineDesc.setPipelineLayout(filterPipelineLayout);

		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT; // TODO: rgb only
		colorBlendAttachment.blendEnable = VK_FALSE;

		filterCubeMapPipelineDesc.addColorBlendAttachment(colorBlendAttachment, 6u);

		//colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
		filterCubeMapPipelineDesc.addColorBlendAttachment(colorBlendAttachment, 1u);

		filterCubeMapPipelineDesc.setViewportExtent(VkExtent2D{ cubeMapSideLength, cubeMapSideLength });

		if (vulkan.createPipeline(filterPipeline, filterCubeMapPipelineDesc.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	const std::vector<VkClearValue> clearValues(6u, { 0.0f, 0.0f, 1.0f, 1.0f });

	VkCommandBuffer cubeMapCmd;
	if (vulkan.createCommandBuffer(cubeMapCmd) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (vulkan.beginCommandBuffer(cubeMapCmd, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	////////////////////////////////////////////////////////////////////////////////////////
	// Transform panorama image to cube map

	if (!inputIsCubemap)
	{
		printf("Transform panorama image to cube map\n");

		res = panoramaToCubemap(vulkan, cubeMapCmd, fullscreenVertexShader, panoramaImage, inputCubeMap);
		if (res != VK_SUCCESS)
		{
			printf("Failed to transform panorama image to cube map\n");
			return res;
		}

		currentInputCubeMapLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	else
	{
		currentInputCubeMapLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}


	////////////////////////////////////////////////////////////////////////////////////////
	//Generate MipLevels
	printf("Generating mipmap levels\n");
	generateMipmapLevels(vulkan, cubeMapCmd, inputCubeMap, maxMipLevels, cubeMapSideLength, currentInputCubeMapLayout);
	currentInputCubeMapLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// Filter

	if (_distribution != IBLLib::Distribution::None)
	{
		switch (_distribution)
		{
			case IBLLib::Distribution::Lambertian:
				printf("Filtering lambertian\n");
				break;
			case IBLLib::Distribution::GGX:
				printf("Filtering GGX\n");
				break;
			case IBLLib::Distribution::Charlie:
				printf("Filtering Charlie\n");
				break;
			default:
				break;
		}

		vulkan.bindDescriptorSet(cubeMapCmd, filterPipelineLayout, filterDescriptorSet);

		vkCmdBindPipeline(cubeMapCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterPipeline);

		// Filter every mip level: from inputCubeMap->currentMipLevel
		// The mip levels are filtered from the smallest mipmap to the largest mipmap,
		// i.e. the last mipmap is filtered last.
		// This has the desirable side effect that the framebuffer size of the last filter pass
		// matches with the LUT size, allowing the LUT to only be written in the last pass
		// without worrying to preserve the LUT's image contents between the previous render passes.
		for (uint32_t currentMipLevel = maxMipLevels - 1; currentMipLevel != -1; currentMipLevel--)
		{
			unsigned int currentFramebufferSideLength = cubeMapSideLength >> currentMipLevel;
			std::vector<VkImageView> renderTargetViews(outputCubeMapViews[currentMipLevel]);

			renderTargetViews.emplace_back(outputLUTView);

			//Framebuffer will be destroyed automatically at shutdown
			VkFramebuffer filterOutputFramebuffer = VK_NULL_HANDLE;
			if (vulkan.createFramebuffer(filterOutputFramebuffer, renderPass, currentFramebufferSideLength, currentFramebufferSideLength, renderTargetViews, 1u) != VK_SUCCESS)
			{
				return Result::VulkanError;
			}

			VkImageSubresourceRange  subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, currentMipLevel, 1u, 0u, 6u };

			vulkan.imageBarrier(cubeMapCmd, outputCubeMap,
													VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
													VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,//src stage, access
													VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // dst stage, access
													subresourceRange);

			PushConstant values{};
			values.roughness = static_cast<float>(currentMipLevel) / static_cast<float>(maxMipLevels - 1);
			values.sampleCount = _sampleCount;
			values.mipLevel = currentMipLevel;
			values.width = cubeMapSideLength;
			values.lodBias = _lodBias;
			values.distribution = _distribution;

			vkCmdPushConstants(cubeMapCmd, filterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstant), &values);

			vulkan.beginRenderPass(cubeMapCmd, renderPass, filterOutputFramebuffer, VkRect2D{ 0u, 0u, currentFramebufferSideLength, currentFramebufferSideLength }, clearValues);
			vkCmdDraw(cubeMapCmd, 3, 1u, 0, 0);
			vulkan.endRenderPass(cubeMapCmd);
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////
	//Output

	VkFormat vulkanTargetFormat;
	switch (_targetFormat) {
	case IBLLib::OutputFormat::R8G8B8A8_UNORM:
		vulkanTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
		break;
	case IBLLib::OutputFormat::R32G32B32A32_SFLOAT:
	case IBLLib::OutputFormat::B9G9R9E5_UFLOAT:
		// The GPU can't write to B9G9R9E5_UFLOAT textures, so convert on the CPU.
		// TODO: Use compute shader?
		vulkanTargetFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
		break;
	case IBLLib::OutputFormat::R16G16B16A16_SFLOAT:
		vulkanTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
		break;
	}

	VkImageLayout currentCubeMapImageLayout;
	VkImage convertedCubeMap = VK_NULL_HANDLE;

	if(_distribution == IBLLib::Distribution::None)
	{
		currentCubeMapImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	else
	{
		currentCubeMapImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	if(vulkanTargetFormat != cubeMapFormat)
	{
		if ((res = convertVkFormat(vulkan, cubeMapCmd, outputCubeMap, convertedCubeMap, vulkanTargetFormat, currentCubeMapImageLayout)) != Success)
		{
			printf("Failed to convert Image \n");
			return res;
		}
		currentCubeMapImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
	else
	{
		convertedCubeMap = outputCubeMap;
	}

	if (vulkan.endCommandBuffer(cubeMapCmd) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (vulkan.executeCommandBuffer(cubeMapCmd) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (downloadCubemap(vulkan, convertedCubeMap, _outputPathCubeMap, static_cast<VkFormat>(_targetFormat), currentCubeMapImageLayout) != VK_SUCCESS)
	{
		printf("Failed to download Image \n");
		return Result::VulkanError;
	}

	if (_outputPathLUT != nullptr)
	{
		if (download2DImage(vulkan, outputLUT, _outputPathLUT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) != VK_SUCCESS)
		{
			printf("Failed to download Image \n");
			return Result::VulkanError;
		}
	}

	return Result::Success;
}

extern "C"
{

IBLLib::Result IBLSample(
	const char* _inputPath,
	const char* _outputPathCubeMap,
	const char* _outputPathLUT,
	IBLLib::Distribution _distribution,
	unsigned int  _cubemapResolution,
	unsigned int _mipmapCount,
	unsigned int _sampleCount,
	IBLLib::OutputFormat _targetFormat,
	float _lodBias,
	bool _debugOutput)
{
	return IBLLib::sample(_inputPath, _outputPathCubeMap, _outputPathLUT, _distribution, _cubemapResolution, _mipmapCount, _sampleCount, _targetFormat, _lodBias, _debugOutput);
}

}
