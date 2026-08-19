/*
 * obs-gstreamer. Steady-lead playout clock.
 *
 * This module deliberately contains only the timing and buffering policy.
 * The OBS/GStreamer adapter lives in gstreamer-source.c.
 */

#pragma once

#include <gst/gst.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct steady_clock steady_clock_t;

struct steady_clock_stats {
	int buffer_fill_ms;
	float output_speed;
	int stream_delay_ms;
	int audio_underruns;
	int clock_reanchors;
	int late_video_frames;
	bool primed;
};

struct steady_clock_callbacks {
	void (*audio)(void *opaque, const float *samples, size_t frames,
		      unsigned channels, unsigned sample_rate, uint64_t timestamp_ns);
	void (*video)(void *opaque, GstSample *sample, uint64_t timestamp_ns);
};

steady_clock_t *steady_clock_create(void *opaque,
				    const struct steady_clock_callbacks *callbacks,
				    unsigned output_rate, int target_ms,
				    bool adaptive_speed);

void steady_clock_destroy(steady_clock_t *clock);
void steady_clock_start(steady_clock_t *clock);
void steady_clock_stop(steady_clock_t *clock);
void steady_clock_reset(steady_clock_t *clock);

bool steady_clock_push_audio(steady_clock_t *clock, const float *samples,
				     size_t frames, unsigned channels,
				     unsigned sample_rate, uint64_t pts_ns);
bool steady_clock_push_video(steady_clock_t *clock, GstSample *sample,
				     uint64_t pts_ns, uint64_t duration_ns);

void steady_clock_get_stats(steady_clock_t *clock,
				    struct steady_clock_stats *stats);
