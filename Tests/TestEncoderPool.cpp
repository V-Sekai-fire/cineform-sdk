/*! @file TestEncoderPool.cpp

*  @brief Encode a short sequence through the asynchronous encoder pool
*
*  The pool runs one encoder per thread and hands finished samples back
*  through a job queue, so this is the path a sanitizer build has to see:
*  build with -fsanitize=thread, or with -fsanitize=address,undefined, and
*  run it. It exits non-zero if a frame is lost, empty or out of order.
*
*  Licensed under either:
*  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
*  - MIT license, http://opensource.org/licenses/MIT
*  at your option.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CFHDEncoder.h"

static const int WIDTH = 320;
static const int HEIGHT = 192;
static const int FRAMES = 48;
static const int THREADS = 4;
static const int QUEUE_LENGTH = 8;

// Take one finished sample and check it is the next frame in order.
static int take_sample(CFHD_EncoderPoolRef pool, uint32_t expected)
{
	uint32_t frame = 0;
	CFHD_SampleBufferRef buffer = NULL;
	CFHD_Error error = CFHD_WaitForSample(pool, &frame, &buffer);
	if (error != CFHD_ERROR_OKAY || buffer == NULL) {
		fprintf(stderr, "frame %u: CFHD_WaitForSample returned %d\n", expected, (int)error);
		return 1;
	}

	void *data = NULL;
	size_t size = 0;
	error = CFHD_GetEncodedSample(buffer, &data, &size);
	int failed = 0;
	if (error != CFHD_ERROR_OKAY || data == NULL || size == 0) {
		fprintf(stderr, "frame %u: empty sample (error %d, %zu bytes)\n", expected, (int)error, size);
		failed = 1;
	} else if (frame != expected) {
		fprintf(stderr, "frame %u: pool returned frame %u\n", expected, frame);
		failed = 1;
	}
	CFHD_ReleaseSampleBuffer(pool, buffer);
	return failed;
}

int main()
{
	const ptrdiff_t pitch = WIDTH * 4;
	// One frame buffer per queue slot: the pool copies nothing, so a frame must stay
	// untouched until its sample has been taken.
	uint8_t *pixels = (uint8_t *)malloc(pitch * HEIGHT * QUEUE_LENGTH);
	if (pixels == NULL) {
		return 1;
	}

	CFHD_EncoderPoolRef pool = NULL;
	if (CFHD_CreateEncoderPool(&pool, THREADS, QUEUE_LENGTH, NULL) != CFHD_ERROR_OKAY ||
		CFHD_PrepareEncoderPool(pool, WIDTH, HEIGHT, CFHD_PIXEL_FORMAT_BGRA,
								CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE,
								CFHD_ENCODING_QUALITY_HIGH) != CFHD_ERROR_OKAY ||
		CFHD_StartEncoderPool(pool) != CFHD_ERROR_OKAY) {
		fprintf(stderr, "could not start the encoder pool\n");
		return 1;
	}

	int failures = 0;
	uint32_t received = 0;
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		// The pool blocks a submission while its queue is full, and only taking a
		// sample frees a slot, so keep no more than QUEUE_LENGTH frames in flight.
		// That also frees this frame's buffer.
		if (frame - received == (uint32_t)QUEUE_LENGTH) {
			failures += take_sample(pool, received++);
		}

		// A different picture per frame so a mixed-up sample cannot pass.
		uint8_t *image = pixels + (frame % QUEUE_LENGTH) * pitch * HEIGHT;
		for (int y = 0; y < HEIGHT; y++) {
			for (int x = 0; x < WIDTH; x++) {
				uint8_t *p = image + y * pitch + x * 4;
				p[0] = (uint8_t)(x + frame * 5);
				p[1] = (uint8_t)(y + frame * 3);
				p[2] = (uint8_t)(frame * 11);
				p[3] = 255;
			}
		}

		if (CFHD_EncodeAsyncSample(pool, frame, image, pitch, NULL) != CFHD_ERROR_OKAY) {
			fprintf(stderr, "frame %u: CFHD_EncodeAsyncSample failed\n", frame);
			return 1;
		}
	}
	while (received < FRAMES) {
		failures += take_sample(pool, received++);
	}

	CFHD_StopEncoderPool(pool);
	CFHD_ReleaseEncoderPool(pool);
	free(pixels);

	printf("%u frames encoded, %d failed\n", received, failures);
	return failures == 0 ? 0 : 1;
}
