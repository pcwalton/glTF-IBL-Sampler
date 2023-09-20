#pragma once

#include <vector>
#include <volk.h>
#include <cstdint>
#include "ResultType.h"

namespace IBLLib
{
	struct KTXHeader {
		uint8_t identifier[12];
		uint32_t vkFormat;
		uint32_t typeSize;
		uint32_t pixelWidth;
		uint32_t pixelHeight;
		uint32_t pixelDepth;
		uint32_t layerCount;
		uint32_t faceCount;
		uint32_t levelCount;
		uint32_t supercompressionScheme;
	};

	struct KTXLevelIndex {
		uint64_t byteOffset;
		uint64_t byteLength;
		uint64_t uncompressedByteLength;
	};

	class KtxImage
	{
	public:
		// use this constructor if you want to create a ktx file
		KtxImage(uint32_t _width, uint32_t _height, VkFormat _vkFormat, uint32_t _levels, bool _isCubeMap);

		Result writeFace(const std::vector<uint8_t>& _inData, uint32_t _side, uint32_t _level);
		Result save(const char* _pathOut);

		uint32_t getWidth() const;
		uint32_t getHeight() const;
		uint32_t getLevels() const;
		bool isCubeMap() const;
		VkFormat getFormat() const;

	private:
		std::vector<uint8_t> mData;
		KTXHeader mHeader;
		std::vector<KTXLevelIndex> mLevelIndices;
	};

} // !IBLLIb
