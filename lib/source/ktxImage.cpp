#include "khr_df.h"
#include "ktxImage.h"

#include <stdio.h>

#include <volk.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <fstream>

using namespace IBLLib;

namespace {

// TODO
uint32_t toLittleEndian(uint32_t n)
{
	return n;
}

// TODO
uint64_t toLittleEndian(uint64_t n)
{
	return n;
}

// Copied and pasted from KTX-Software/lib/dfdutils/createdfd.c
namespace dfd {

/** Qualifier suffix to the format, in Vulkan terms. */
enum VkSuffix {
    s_UNORM,   /*!< Unsigned normalized format. */
    s_SNORM,   /*!< Signed normalized format. */
    s_USCALED, /*!< Unsigned scaled format. */
    s_SSCALED, /*!< Signed scaled format. */
    s_UINT,    /*!< Unsigned integer format. */
    s_SINT,    /*!< Signed integer format. */
    s_SFLOAT,  /*!< Signed float format. */
    s_UFLOAT,  /*!< Unsigned float format. */
    s_SRGB     /*!< sRGB normalized format. */
};

typedef enum { i_COLOR, i_NON_COLOR } channels_infotype;

// This doesn't include the length word.
size_t dfdWordSize(int numSamples)
{
	return KHR_DF_WORD_SAMPLESTART + numSamples * KHR_DF_WORD_SAMPLEWORDS;
}

static uint32_t *writeHeader(int numSamples, int bytes, int suffix,
                             channels_infotype infotype)
{
    uint32_t *DFD = (uint32_t *) malloc(sizeof(uint32_t) *
                                        (1 + KHR_DF_WORD_SAMPLESTART +
                                         numSamples * KHR_DF_WORD_SAMPLEWORDS));
    uint32_t* BDFD = DFD+1;
    DFD[0] = sizeof(uint32_t) *
        (1 + KHR_DF_WORD_SAMPLESTART +
         numSamples * KHR_DF_WORD_SAMPLEWORDS);
    BDFD[KHR_DF_WORD_VENDORID] =
        (KHR_DF_VENDORID_KHRONOS << KHR_DF_SHIFT_VENDORID) |
        (KHR_DF_KHR_DESCRIPTORTYPE_BASICFORMAT << KHR_DF_SHIFT_DESCRIPTORTYPE);
    BDFD[KHR_DF_WORD_VERSIONNUMBER] =
        (KHR_DF_VERSIONNUMBER_LATEST << KHR_DF_SHIFT_VERSIONNUMBER) |
        (((uint32_t)sizeof(uint32_t) *
          (KHR_DF_WORD_SAMPLESTART +
           numSamples * KHR_DF_WORD_SAMPLEWORDS)
          << KHR_DF_SHIFT_DESCRIPTORBLOCKSIZE));
    BDFD[KHR_DF_WORD_MODEL] =
        ((KHR_DF_MODEL_RGBSDA << KHR_DF_SHIFT_MODEL) | /* Only supported model */
         (KHR_DF_FLAG_ALPHA_STRAIGHT << KHR_DF_SHIFT_FLAGS));
    if (infotype == i_COLOR) {
        BDFD[KHR_DF_WORD_PRIMARIES] |= KHR_DF_PRIMARIES_BT709 << KHR_DF_SHIFT_PRIMARIES; /* Assumed */
    } else {
        BDFD[KHR_DF_WORD_PRIMARIES] |= KHR_DF_PRIMARIES_UNSPECIFIED << KHR_DF_SHIFT_PRIMARIES;
    }
    if (suffix == s_SRGB) {
        BDFD[KHR_DF_WORD_TRANSFER] |= KHR_DF_TRANSFER_SRGB << KHR_DF_SHIFT_TRANSFER;
    } else {
        BDFD[KHR_DF_WORD_TRANSFER] |= KHR_DF_TRANSFER_LINEAR << KHR_DF_SHIFT_TRANSFER;
    }
    BDFD[KHR_DF_WORD_TEXELBLOCKDIMENSION0] = 0; /* Only 1x1x1x1 texel blocks supported */
    BDFD[KHR_DF_WORD_BYTESPLANE0] = bytes; /* bytesPlane0 = bytes, bytesPlane3..1 = 0 */
    BDFD[KHR_DF_WORD_BYTESPLANE4] = 0; /* bytesPlane7..5 = 0 */
    return DFD;
}

static uint32_t setChannelFlags(uint32_t channel, enum VkSuffix suffix)
{
    switch (suffix) {
    case s_UNORM: break;
    case s_SNORM:
        channel |=
            KHR_DF_SAMPLE_DATATYPE_SIGNED;
        break;
    case s_USCALED: break;
    case s_SSCALED:
        channel |=
            KHR_DF_SAMPLE_DATATYPE_SIGNED;
        break;
    case s_UINT: break;
    case s_SINT:
        channel |=
            KHR_DF_SAMPLE_DATATYPE_SIGNED;
        break;
    case s_SFLOAT:
        channel |=
            KHR_DF_SAMPLE_DATATYPE_FLOAT |
            KHR_DF_SAMPLE_DATATYPE_SIGNED;
        break;
    case s_UFLOAT:
        channel |=
            KHR_DF_SAMPLE_DATATYPE_FLOAT;
        break;
    case s_SRGB:
        if (channel == KHR_DF_CHANNEL_RGBSDA_ALPHA) {
            channel |= KHR_DF_SAMPLE_DATATYPE_LINEAR;
        }
        break;
    }
    return channel;
}

static void writeSample(uint32_t *DFD, int sampleNo, int channel,
                        int bits, int offset,
                        int topSample, int bottomSample, enum VkSuffix suffix)
{
    // Use this to avoid type-punning complaints from the gcc optimizer
    // with -Wall.
    union {
        uint32_t i;
        float f;
    } lower, upper;
    uint32_t *sample = DFD + 1 + KHR_DF_WORD_SAMPLESTART + sampleNo * KHR_DF_WORD_SAMPLEWORDS;
    if (channel == 3) channel = KHR_DF_CHANNEL_RGBSDA_ALPHA;

    if (channel == 3) channel = KHR_DF_CHANNEL_RGBSDA_ALPHA;
    channel = setChannelFlags(channel, suffix);

    sample[KHR_DF_SAMPLEWORD_BITOFFSET] =
        (offset << KHR_DF_SAMPLESHIFT_BITOFFSET) |
        ((bits - 1) << KHR_DF_SAMPLESHIFT_BITLENGTH) |
        (channel << KHR_DF_SAMPLESHIFT_CHANNELID);

    sample[KHR_DF_SAMPLEWORD_SAMPLEPOSITION_ALL] = 0;

    switch (suffix) {
    case s_UNORM:
    case s_SRGB:
    default:
        if (bits > 32) {
            upper.i = 0xFFFFFFFFU;
        } else {
            upper.i = (uint32_t)((1U << bits) - 1U);
        }
        lower.i = 0U;
        break;
    case s_SNORM:
        if (bits > 32) {
            upper.i = 0x7FFFFFFF;
        } else {
            upper.i = topSample ? (1U << (bits - 1)) - 1 : (1U << bits) - 1;
        }
        lower.i = ~upper.i;
        if (bottomSample) lower.i += 1;
        break;
    case s_USCALED:
    case s_UINT:
        upper.i = bottomSample ? 1U : 0U;
        lower.i = 0U;
        break;
    case s_SSCALED:
    case s_SINT:
        upper.i = bottomSample ? 1U : 0U;
        lower.i = ~0U;
        break;
    case s_SFLOAT:
        upper.f = 1.0f;
        lower.f = -1.0f;
        break;
    case s_UFLOAT:
        upper.f = 1.0f;
        lower.f = 0.0f;
        break;
    }
    sample[KHR_DF_SAMPLEWORD_SAMPLELOWER] = lower.i;
    sample[KHR_DF_SAMPLEWORD_SAMPLEUPPER] = upper.i;
}

/**
 * @~English
 * @brief Create a Data Format Descriptor for an unpacked format.
 *
 * @param bigEndian Set to 1 for big-endian byte ordering and
                    0 for little-endian byte ordering.
 * @param numChannels The number of color channels.
 * @param bytes The number of bytes per channel.
 * @param redBlueSwap Normally channels appear in consecutive R, G, B, A order
 *                    in memory; redBlueSwap inverts red and blue, allowing
 *                    B, G, R, A.
 * @param suffix Indicates the format suffix for the type.
 *
 * @return A data format descriptor in malloc'd data. The caller is responsible
 *         for freeing the descriptor.
 **/
uint32_t *createDFDUnpacked(int bigEndian, int numChannels, int bytes,
                            int redBlueSwap, enum VkSuffix suffix)
{
    uint32_t *DFD;
    if (bigEndian) {
        int channelCounter, channelByte;
        /* Number of samples = number of channels * bytes per channel */
        DFD = writeHeader(numChannels * bytes, numChannels * bytes, suffix, i_COLOR);
        /* First loop over the channels */
        for (channelCounter = 0; channelCounter < numChannels; ++channelCounter) {
            int channel = channelCounter;
            if (redBlueSwap && (channel == 0 || channel == 2)) {
                channel ^= 2;
            }
            /* Loop over the bytes that constitute a channel */
            for (channelByte = 0; channelByte < bytes; ++channelByte) {
                writeSample(DFD, channelCounter * bytes + channelByte, channel,
                            8, 8 * (channelCounter * bytes + bytes - channelByte - 1),
                            channelByte == bytes-1, channelByte == 0, suffix);
            }
        }

    } else { /* Little-endian */

        int sampleCounter;
        /* One sample per channel */
        DFD = writeHeader(numChannels, numChannels * bytes, suffix, i_COLOR);
        for (sampleCounter = 0; sampleCounter < numChannels; ++sampleCounter) {
            int channel = sampleCounter;
            if (redBlueSwap && (channel == 0 || channel == 2)) {
                channel ^= 2;
            }
            writeSample(DFD, sampleCounter, channel,
                        8 * bytes, 8 * sampleCounter * bytes,
                        1, 1, suffix);
        }
    }
    return DFD;
}

/**
 * @~English
 * @brief Create a Data Format Descriptor for a packed format.
 *
 * @param bigEndian Big-endian flag: Set to 1 for big-endian byte ordering and
 *                  0 for little-endian byte ordering.
 * @param numChannels The number of color channels.
 * @param bits[] An array of length numChannels.
 *               Each entry is the number of bits composing the channel, in
 *               order starting at bit 0 of the packed type.
 * @param channels[] An array of length numChannels.
 *                   Each entry enumerates the channel type: 0 = red, 1 = green,
 *                   2 = blue, 15 = alpha, in order starting at bit 0 of the
 *                   packed type. These values match channel IDs for RGBSDA in
 *                   the Khronos Data Format header. To simplify iteration
 *                   through channels, channel id 3 is a synonym for alpha.
 * @param suffix Indicates the format suffix for the type.
 *
 * @return A data format descriptor in malloc'd data. The caller is responsible
 *         for freeing the descriptor.
 **/
uint32_t *createDFDPacked(int bigEndian, int numChannels,
                          int bits[], int channels[],
                          enum VkSuffix suffix)
{
    uint32_t *DFD = 0;
    if (numChannels == 6) {
        /* Special case E5B9G9R9 */
        DFD = writeHeader(numChannels, 4, s_UFLOAT, i_COLOR);
        writeSample(DFD, 0, 0,
                    9, 0,
                    1, 1, s_UNORM);
        KHR_DFDSETSVAL((DFD+1), 0, SAMPLEUPPER, 8448);
        writeSample(DFD, 1, 0 | KHR_DF_SAMPLE_DATATYPE_EXPONENT,
                    5, 27,
                    1, 1, s_UNORM);
        KHR_DFDSETSVAL((DFD+1), 1, SAMPLELOWER, 15);
        KHR_DFDSETSVAL((DFD+1), 1, SAMPLEUPPER, 31);
        writeSample(DFD, 2, 1,
                    9, 9,
                    1, 1, s_UNORM);
        KHR_DFDSETSVAL((DFD+1), 2, SAMPLEUPPER, 8448);
        writeSample(DFD, 3, 1 | KHR_DF_SAMPLE_DATATYPE_EXPONENT,
                    5, 27,
                    1, 1, s_UNORM);
        KHR_DFDSETSVAL((DFD+1), 3, SAMPLELOWER, 15);
        KHR_DFDSETSVAL((DFD+1), 3, SAMPLEUPPER, 31);
        writeSample(DFD, 4, 2,
                    9, 18,
                    1, 1, s_UNORM);
        KHR_DFDSETSVAL((DFD+1), 4, SAMPLEUPPER, 8448);
        writeSample(DFD, 5, 2 | KHR_DF_SAMPLE_DATATYPE_EXPONENT,
                    5, 27,
                    1, 1, s_UNORM);
        KHR_DFDSETSVAL((DFD+1), 5, SAMPLELOWER, 15);
        KHR_DFDSETSVAL((DFD+1), 5, SAMPLEUPPER, 31);
    } else if (bigEndian) {
        /* No packed format is larger than 32 bits. */
        /* No packed channel crosses more than two bytes. */
        int totalBits = 0;
        int bitChannel[32];
        int beChannelStart[4];
        int channelCounter;
        int bitOffset = 0;
        int BEMask;
        int numSamples = numChannels;
        int sampleCounter;
        for (channelCounter = 0; channelCounter < numChannels; ++channelCounter) {
            beChannelStart[channelCounter] = totalBits;
            totalBits += bits[channelCounter];
        }
        BEMask = (totalBits - 1) & 0x18;
        for (channelCounter = 0; channelCounter < numChannels; ++channelCounter) {
            bitChannel[bitOffset ^ BEMask] = channelCounter;
            if (((bitOffset + bits[channelCounter] - 1) & ~7) != (bitOffset & ~7)) {
                /* Continuation sample */
                bitChannel[((bitOffset + bits[channelCounter] - 1) & ~7) ^ BEMask] = channelCounter;
                numSamples++;
            }
            bitOffset += bits[channelCounter];
        }
        DFD = writeHeader(numSamples, totalBits >> 3, suffix, i_COLOR);

        sampleCounter = 0;
        for (bitOffset = 0; bitOffset < totalBits;) {
            if (bitChannel[bitOffset] == -1) {
                /* Done this bit, so this is the lower half of something. */
                /* We must therefore jump to the end of the byte and continue. */
                bitOffset = (bitOffset + 8) & ~7;
            } else {
                /* Start of a channel? */
                int thisChannel = bitChannel[bitOffset];
                if ((beChannelStart[thisChannel] ^ BEMask) == bitOffset) {
                    /* Must be just one sample if we hit it first. */
                    writeSample(DFD, sampleCounter++, channels[thisChannel],
                                    bits[thisChannel], bitOffset,
                                    1, 1, suffix);
                    bitOffset += bits[thisChannel];
                } else {
                    /* Two samples. Move to the end of the first one we hit when we're done. */
                    int firstSampleBits = 8 - (beChannelStart[thisChannel] & 0x7); /* Rest of the byte */
                    int secondSampleBits = bits[thisChannel] - firstSampleBits; /* Rest of the bits */
                    writeSample(DFD, sampleCounter++, channels[thisChannel],
                                firstSampleBits, beChannelStart[thisChannel] ^ BEMask,
                                0, 1, suffix);
                    /* Mark that we've already handled this sample */
                    bitChannel[beChannelStart[thisChannel] ^ BEMask] = -1;
                    writeSample(DFD, sampleCounter++, channels[thisChannel],
                                secondSampleBits, bitOffset,
                                1, 0, suffix);
                    bitOffset += secondSampleBits;
                }
            }
        }

    } else { /* Little-endian */

        int sampleCounter;
        int totalBits = 0;
        int bitOffset = 0;
        for (sampleCounter = 0; sampleCounter < numChannels; ++sampleCounter) {
            totalBits += bits[sampleCounter];
        }

        /* One sample per channel */
        DFD = writeHeader(numChannels, totalBits >> 3, suffix, i_COLOR);
        for (sampleCounter = 0; sampleCounter < numChannels; ++sampleCounter) {
            writeSample(DFD, sampleCounter, channels[sampleCounter],
                        bits[sampleCounter], bitOffset,
                        1, 1, suffix);
            bitOffset += bits[sampleCounter];
        }
    }
    return DFD;
}

}	// end namespace dfd

}	// end anonymous namespace

KtxImage::KtxImage(uint32_t _width, uint32_t _height, VkFormat _vkFormat, uint32_t _levels, bool _isCubeMap)
{
	uint32_t *dfdData;
	size_t bytesPerPixel;

	switch (_vkFormat) {
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		dfdData = dfd::createDFDUnpacked(0, 4, 4, 0, dfd::s_SFLOAT);
		bytesPerPixel = 16;
		break;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		dfdData = dfd::createDFDUnpacked(0, 4, 2, 0, dfd::s_SFLOAT);
		bytesPerPixel = 8;
		break;
	case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: {
        int channels[1] = {0};
        int bits[1] = {0};
        dfdData = dfd::createDFDPacked(0, 6, bits, channels, dfd::s_UFLOAT);
		bytesPerPixel = 4;
		break;
    }
	case VK_FORMAT_R8G8B8A8_UNORM:
		dfdData = dfd::createDFDUnpacked(0, 4, 1, 0, dfd::s_UNORM);
		bytesPerPixel = 4;
		break;
	default:
		assert(0 && "Unsupported Vulkan format");
	}

	memcpy(mHeader.identifier, KTX_IDENTIFIER, sizeof(KTX_IDENTIFIER));
	mHeader.vkFormat = _vkFormat;
	mHeader.typeSize = 4;
	mHeader.pixelWidth = _width;
	mHeader.pixelHeight = _height;
	mHeader.pixelDepth = 0;
	mHeader.layerCount = 0;
	mHeader.faceCount = _isCubeMap ? 6 : 1;
	mHeader.levelCount = _levels;
	mHeader.supercompressionScheme = 0;
	mData.insert(
		mData.end(),
		reinterpret_cast<uint8_t *>(&mHeader),
		reinterpret_cast<uint8_t *>(&mHeader + 1));

	size_t indexOffset = mData.size();
	KTXIndex index = {0};
	mData.insert(
		mData.end(), reinterpret_cast<uint8_t *>(&index), reinterpret_cast<uint8_t *>(&index + 1));

	// Write placeholder level indices.
	size_t levelIndexOffset = mData.size();
	mLevelIndices.resize(_levels);
	mData.insert(
		mData.end(),
		reinterpret_cast<uint8_t *>(&mLevelIndices[0]),
		reinterpret_cast<uint8_t *>(&mLevelIndices[0] + _levels));

	index.dfdByteOffset = static_cast<uint32_t>(mData.size());
	uint32_t wordSize = static_cast<uint32_t>(dfd::dfdWordSize(4));
	index.dfdByteLength = (wordSize + 1) * 4;
	mData.insert(
		mData.end(),
		reinterpret_cast<uint8_t *>(dfdData),
		reinterpret_cast<uint8_t *>(dfdData + wordSize));

	index.kvdByteOffset = 0;
	index.kvdByteLength = 0;
	index.sgdByteOffset = 0;
	index.sgdByteLength = 0;
	memcpy(&mData[indexOffset], &index, sizeof(index));

	// Compute mip sizes.
	size_t mipWidth = mHeader.pixelWidth, mipHeight = mHeader.pixelHeight;
	for (size_t mip = 0; mip < _levels; mip++) {
		size_t mipLength = mipWidth * mipHeight * mHeader.faceCount * bytesPerPixel;

		mLevelIndices[mip].byteLength = mLevelIndices[mip].uncompressedByteLength = mipLength;

		mipWidth /= 2;
		mipHeight /= 2;
	}

	// Compute mip lengths.
	size_t mipOffset = mData.size();
	for (int mip = _levels - 1; mip >= 0; mip--) {
		if ((mipOffset & 0xf) != 0)
			mipOffset += 16 - (mipOffset & 0xf);
		mLevelIndices[mip].byteOffset = mipOffset;
		mipOffset += mLevelIndices[mip].byteLength;
	}

	// Copy in mip level data.
	memcpy(&mData[levelIndexOffset], &mLevelIndices[0], _levels * sizeof(mLevelIndices[0]));

	// Reserve space for mip levels.
	mData.resize(mipOffset);
}

Result KtxImage::writeFace(const std::vector<uint8_t>& _inData, uint32_t _side, uint32_t _level)
{
	assert(_side < mHeader.faceCount && "Side out of range");
	assert(_level < mHeader.levelCount && "Level out of range");

	const KTXLevelIndex &levelIndex = mLevelIndices[_level];
	size_t mipFaceSize = levelIndex.uncompressedByteLength / mHeader.faceCount;
	assert(_inData.size() == mipFaceSize && "Face size has an incorrect length");

	memcpy(&mData[levelIndex.byteOffset + mipFaceSize * _side], &_inData[0], mipFaceSize);

	return Success;
}

Result KtxImage::save(const char* _pathOut)
{
	std::ofstream out;
	out.open(_pathOut, std::ios::out | std::ios::trunc | std::ios::binary);
	out.write((const char *)&mData[0], mData.size());
	out.close();

	return Success;
}

uint32_t KtxImage::getWidth() const
{
	return mHeader.pixelWidth;
}

uint32_t KtxImage::getHeight() const
{
	return mHeader.pixelHeight;
}

uint32_t KtxImage::getLevels() const
{
	return mHeader.levelCount;
}

bool KtxImage::isCubeMap() const
{
	return mHeader.faceCount == 6;
}

VkFormat KtxImage::getFormat() const
{
	return static_cast<VkFormat>(mHeader.vkFormat);
}
