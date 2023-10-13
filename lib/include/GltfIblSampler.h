#pragma once
#include "ResultType.h"

namespace IBLLib
{
	enum class OutputFormat
	{
		R8G8B8A8_UNORM = 37,
		R16G16B16A16_SFLOAT = 97,
		R32G32B32A32_SFLOAT = 109,
		B9G9R9E5_UFLOAT = 123
	};

	enum class Distribution : unsigned int 
	{
		None = 0,
		Lambertian = 1,
		GGX = 2,
		Charlie = 3
	};

	Result sample(const char* _inputPath, const char* _outputPathCubeMap, const char* _outputPathLUT, Distribution _distribution, unsigned int  _cubemapResolution, unsigned int _mipmapCount, unsigned int _sampleCount, OutputFormat _targetFormat, float _lodBias, bool _debugOutput);
} // !IBLLib

extern "C"
{

// Cross-compiler-ABI version of the above.
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
	bool _debugOutput);

}	// extern "C"