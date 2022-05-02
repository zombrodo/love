/**
 * Copyright (c) 2006-2022 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "MP3Decoder.h"
#include "common/Exception.h"

namespace love
{
namespace sound
{
namespace lullaby
{

static size_t onRead(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
	auto decoder = (MP3Decoder *) pUserData;
	int64 read = decoder->stream->read(pBufferOut, bytesToRead);
	return std::max<int64>(0, read);
}

static drmp3_bool32 onSeek(void *pUserData, int offset, drmp3_seek_origin origin)
{
	auto decoder = (MP3Decoder *) pUserData;
	int64 pos = decoder->offset;

	// Due to possible offsets, we have to calculate the position ourself.
	switch (origin)
	{
	case drmp3_seek_origin_start:
		pos += offset;
		break;
	case drmp3_seek_origin_current:
		pos = decoder->stream->tell() + offset;
		break;
	default:
		return false;
	}

	if (pos < decoder->offset)
		return false;

	return decoder->stream->seek(pos, Stream::SEEKORIGIN_BEGIN) ? DRMP3_TRUE : DRMP3_FALSE;
}

// Copied from dr_mp3 function drmp3_hdr_valid()
static bool isMP3HeaderValid(const uint8 *h)
{
	return
		// Sync bits
		h[0] == 0xff &&
		((h[1] & 0xF0) == 0xf0 || (h[1] & 0xFE) == 0xe2) &&
		// Check layer
		(DRMP3_HDR_GET_LAYER(h) != 0) &&
		// Check bitrate
		(DRMP3_HDR_GET_BITRATE(h) != 15) &&
		// Check sample rate
		(DRMP3_HDR_GET_SAMPLE_RATE(h) != 3);
}

static int64 findFirstValidHeader(Stream* stream)
{
	constexpr size_t LOOKUP_SIZE = 16384;

	std::vector<uint8> data(LOOKUP_SIZE);
	uint8 header[10];
	uint8 *dataPtr = data.data();
	int64 buffer = 0;
	int64 offset = 0;

	if (stream->read(header, 10) < 10)
		return -1;

	if (memcmp(header, "TAG", 3) == 0)
	{
		// ID3v1 tag is always 128 bytes long
		if (!stream->seek(128, Stream::SEEKORIGIN_BEGIN))
			return -1;

		buffer = stream->read(dataPtr, LOOKUP_SIZE);
		offset = 128;
	}
	else if (memcmp(header, "ID3", 3) == 0)
	{
		// ID3v2 tag header is 10 bytes long, but we're
		// only interested on how much we should skip.
		int64 off =
			header[9] |
			((int64) header[8] << 7) |
			((int64) header[7] << 14) |
			((int64) header[6] << 21);

		if (!stream->seek(off, Stream::SEEKORIGIN_CURRENT))
			return -1;

		buffer = stream->read(dataPtr, LOOKUP_SIZE);
		offset = off + 10;
	}
	else
	{
		// Copy the rest to data buffer
		memcpy(dataPtr, header, 10);
		buffer = 10 + stream->read(dataPtr + 10, LOOKUP_SIZE - 10);
	}

	// Look for mp3 data
	for (int i = 0; i < buffer - 4; i++, offset++)
	{
		if (isMP3HeaderValid(dataPtr++))
		{
			stream->seek(offset, Stream::SEEKORIGIN_BEGIN);
			return offset;
		}
	}

	// No valid MP3 frame found in first 16KB data
	return -1;
}

MP3Decoder::MP3Decoder(Stream *stream, int bufferSize)
: Decoder(stream, bufferSize)
{
	// Check for possible ID3 tag and skip it if necessary.
	offset = findFirstValidHeader(stream);
	if (offset == -1)
		throw love::Exception("Could not find first valid mp3 header.");

	// initialize mp3 handle
	if (!drmp3_init(&mp3, onRead, onSeek, this, nullptr, nullptr))
		throw love::Exception("Could not read mp3 data.");

	sampleRate = mp3.sampleRate;

	// calculate duration
	drmp3_uint64 pcmCount, mp3FrameCount;
	if (!drmp3_get_mp3_and_pcm_frame_count(&mp3, &mp3FrameCount, &pcmCount))
	{
		drmp3_uninit(&mp3);
		throw love::Exception("Could not calculate mp3 duration.");
	}
	duration = ((double) pcmCount) / ((double) mp3.sampleRate);

	// create seek table
	drmp3_uint32 mp3FrameInt = (drmp3_uint32) mp3FrameCount;
	seekTable.resize((size_t) mp3FrameCount, {0ULL, 0ULL, 0, 0});
	if (!drmp3_calculate_seek_points(&mp3, &mp3FrameInt, seekTable.data()))
	{
		drmp3_uninit(&mp3);
		throw love::Exception("Could not calculate mp3 seek table");
	}

	// bind seek table
	if (!drmp3_bind_seek_table(&mp3, mp3FrameInt, seekTable.data()))
	{
		drmp3_uninit(&mp3);
		throw love::Exception("Could not bind mp3 seek table");
	}
}

MP3Decoder::~MP3Decoder()
{
	drmp3_uninit(&mp3);
}

love::sound::Decoder *MP3Decoder::clone()
{
	StrongRef<Stream> s(stream->clone(), Acquire::NORETAIN);
	return new MP3Decoder(s, bufferSize);
}

int MP3Decoder::decode()
{
	// bufferSize is in char
	int maxRead = bufferSize / sizeof(int16_t) / mp3.channels;
	int read = (int) drmp3_read_pcm_frames_s16(&mp3, maxRead, (drmp3_int16 *) buffer);

	if (read < maxRead)
		eof = true;

	return read * sizeof(int16_t) * mp3.channels;
}

bool MP3Decoder::seek(double s)
{
	drmp3_uint64 targetSample = (drmp3_uint64) (s * mp3.sampleRate);
	drmp3_bool32 success = drmp3_seek_to_pcm_frame(&mp3, targetSample);

	if (success)
		eof = false;

	return success;
}

bool MP3Decoder::rewind()
{
	return seek(0.0);
}

bool MP3Decoder::isSeekable()
{
	return true;
}

int MP3Decoder::getChannelCount() const
{
	return mp3.channels;
}

int MP3Decoder::getBitDepth() const
{
	return 16;
}

double MP3Decoder::getDuration()
{
	return duration;
}

} // lullaby
} // sound
} // love
