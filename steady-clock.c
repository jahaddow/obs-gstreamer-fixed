/*
 * obs-gstreamer. Steady-lead playout clock.
 *
 * Copyright (C) 2026 James Haddow
 *
 * This file is part of obs-gstreamer and is licensed under the GPL-2.0-or-later.
 */

#include "steady-clock.h"

#include <gst/audio/audio.h>

#include <math.h>
#include <string.h>

#define STEADY_OUTPUT_LEAD_MS 80
#define STEADY_AUDIO_CHUNK_MS 20
#define STEADY_SPEED_MIN 0.98f
#define STEADY_SPEED_MAX 1.05f
#define STEADY_SPEED_DEADBAND_MS 20
#define STEADY_SPEED_SMOOTHING 0.05f
#define STEADY_MIN_BUFFER_MS 20
#define STEADY_MAX_BUFFER_EXTRA_MS 200
#define STEADY_BUFFER_CAP_MULTIPLIER 4
#define STEADY_MAX_VIDEO_QUEUE 8
#define STEADY_MAX_LAG_MS 1500
#define STEADY_PTS_RESET_MS 2000
#define STEADY_SILENCE_FADE_MS 5
#define STEADY_VIDEO_INTERVAL_NS 16666667ULL
#define STEADY_VIDEO_LATE_TOLERANCE_NS 100000000ULL

typedef struct {
	float *samples;
	size_t frames;
	size_t offset;
	unsigned channels;
	unsigned sample_rate;
	uint64_t pts_ns;
} audio_chunk_t;

typedef struct {
	GstSample *sample;
	uint64_t pts_ns;
	uint64_t duration_ns;
} video_frame_t;

struct steady_clock {
	void *opaque;
	struct steady_clock_callbacks callbacks;

	GMutex mutex;
	GCond cond;
	GThread *thread;
	bool stop_requested;
	bool started;

	GQueue *audio;
	GQueue *video;
	size_t audio_frames;
	unsigned audio_channels;
	unsigned audio_rate;
	unsigned output_rate;
	int target_ms;
	int min_ms;
	int max_ms;
	bool adaptive_speed;

	bool primed;
	uint64_t output_anchor_ns;
	uint64_t output_samples;
	uint64_t next_audio_ns;
	uint64_t latest_input_audio_end_ns;
	uint64_t latest_output_audio_end_ns;
	uint64_t last_input_pts_ns;
	uint64_t input_anchor_system_ns;
	uint64_t input_anchor_pts_ns;
	uint64_t last_audio_push_system_ns;
	bool input_anchor_valid;
	bool stall_reanchored;
	uint64_t video_anchor_system_ns;
	uint64_t video_anchor_pts_ns;
	bool video_anchor_valid;
	uint64_t playout_input_anchor_pts_ns;
	bool playout_anchor_valid;
	uint64_t next_video_ns;

	float output_speed;
	GstAudioResampler *resampler;
	unsigned resampler_channels;
	unsigned resampler_rate;
	unsigned resampler_in_rate;
	float *input_scratch;
	size_t input_scratch_frames;
	float *output_scratch;
	size_t output_scratch_frames;
	float *last_output;
	unsigned last_output_channels;
	bool last_output_was_silence;

	struct steady_clock_stats stats;
};

static uint64_t monotonic_ns(void)
{
	return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static uint64_t frames_to_ns(size_t frames, unsigned rate)
{
	if (rate == 0)
		return 0;
	return (uint64_t)((long double)frames * 1000000000.0L / rate);
}

static int fill_ms_locked(const steady_clock_t *clock)
{
	if (clock->audio_rate == 0)
		return 0;
	return (int)(frames_to_ns(clock->audio_frames, clock->audio_rate) / 1000000ULL);
}

static void free_audio_chunk(audio_chunk_t *chunk)
{
	if (!chunk)
		return;
	g_free(chunk->samples);
	g_free(chunk);
}

static void free_video_frame(video_frame_t *frame)
{
	if (!frame)
		return;
	if (frame->sample)
		gst_sample_unref(frame->sample);
	g_free(frame);
}

static void clear_queues_locked(steady_clock_t *clock)
{
	while (!g_queue_is_empty(clock->audio))
		free_audio_chunk(g_queue_pop_head(clock->audio));
	while (!g_queue_is_empty(clock->video))
		free_video_frame(g_queue_pop_head(clock->video));
	clock->audio_frames = 0;
}

static void reset_resampler_locked(steady_clock_t *clock)
{
	if (clock->resampler)
		gst_audio_resampler_free(clock->resampler);
	clock->resampler = NULL;
	clock->resampler_channels = 0;
	clock->resampler_rate = 0;
	clock->resampler_in_rate = 0;
}

static bool ensure_resampler_locked(steady_clock_t *clock, unsigned channels,
					unsigned input_rate)
{
	if (channels == 0 || input_rate == 0 || clock->output_rate == 0)
		return false;

	if (!clock->resampler || clock->resampler_channels != channels ||
	    clock->resampler_rate != clock->output_rate) {
		reset_resampler_locked(clock);
		clock->resampler = gst_audio_resampler_new(
			GST_AUDIO_RESAMPLER_METHOD_KAISER,
			GST_AUDIO_RESAMPLER_FLAG_VARIABLE_RATE,
			GST_AUDIO_FORMAT_F32LE, (gint)channels, (gint)clock->output_rate,
			(gint)clock->output_rate, NULL);
		if (!clock->resampler)
			return false;
		clock->resampler_channels = channels;
		clock->resampler_rate = clock->output_rate;
		clock->resampler_in_rate = clock->output_rate;
	}

	if (input_rate != clock->resampler_in_rate) {
		if (!gst_audio_resampler_update(clock->resampler, (gint)input_rate,
						(gint)clock->output_rate, NULL))
			return false;
		clock->resampler_in_rate = input_rate;
	}

	return true;
}

static bool ensure_scratch(float **buffer, size_t *capacity,
				   size_t frames, unsigned channels)
{
	if (frames <= *capacity)
		return true;

	size_t samples = frames * channels;
	float *next = g_realloc(*buffer, samples * sizeof(float));
	if (!next)
		return false;
	*buffer = next;
	*capacity = frames;
	return true;
}

static bool ensure_last_output_locked(steady_clock_t *clock, unsigned channels)
{
	if (channels == clock->last_output_channels && clock->last_output)
		return true;
	float *next = g_realloc(clock->last_output, channels * sizeof(float));
	if (!next)
		return false;
	if (channels > clock->last_output_channels)
		memset(next + clock->last_output_channels, 0,
		       (channels - clock->last_output_channels) * sizeof(float));
	clock->last_output = next;
	clock->last_output_channels = channels;
	return true;
}

static size_t copy_audio_locked(steady_clock_t *clock, float *out,
					size_t frames, unsigned channels,
					uint64_t *first_pts_ns)
{
	size_t copied = 0;
	bool first = true;

	while (copied < frames && !g_queue_is_empty(clock->audio)) {
		audio_chunk_t *chunk = g_queue_peek_head(clock->audio);
		if (chunk->channels != channels) {
			free_audio_chunk(g_queue_pop_head(clock->audio));
			continue;
		}

		size_t available = chunk->frames - chunk->offset;
		size_t take = MIN(available, frames - copied);
		if (first && first_pts_ns)
			*first_pts_ns = chunk->pts_ns + frames_to_ns(chunk->offset, chunk->sample_rate);
		first = false;
		memcpy(out + copied * channels, chunk->samples + chunk->offset * channels,
		       take * channels * sizeof(float));
		chunk->offset += take;
		copied += take;
		clock->audio_frames -= take;

		if (chunk->offset == chunk->frames)
			free_audio_chunk(g_queue_pop_head(clock->audio));
	}

	return copied;
}

static float compute_speed_locked(steady_clock_t *clock, int fill_ms)
{
	float target = 1.0f;
	if (clock->adaptive_speed) {
		int low_edge = clock->target_ms - STEADY_SPEED_DEADBAND_MS;
		int high_edge = clock->target_ms + STEADY_SPEED_DEADBAND_MS;
		if (fill_ms < low_edge) {
			int span = MAX(1, low_edge - clock->min_ms);
			float t = (float)(low_edge - fill_ms) / span;
			t = CLAMP(t, 0.0f, 1.0f);
			target = 1.0f - (1.0f - STEADY_SPEED_MIN) * t;
		} else if (fill_ms > high_edge) {
			int span = MAX(1, clock->max_ms - high_edge);
			float t = (float)(fill_ms - high_edge) / span;
			t = CLAMP(t, 0.0f, 1.0f);
			target = 1.0f + (STEADY_SPEED_MAX - 1.0f) * t;
		}
	}

	clock->output_speed += (target - clock->output_speed) * STEADY_SPEED_SMOOTHING;
	clock->output_speed = CLAMP(clock->output_speed, STEADY_SPEED_MIN,
					STEADY_SPEED_MAX);
	return clock->output_speed;
}

static uint64_t video_due_ns_locked(steady_clock_t *clock,
					uint64_t pts_ns)
{
	/* Keep the video mapping fixed for the duration of a playout epoch. The
	 * old implementation recomputed this from the latest audio chunk for every
	 * video frame. A small audio-rate correction could therefore move the video
	 * deadline while the frame was queued, making frames appear late or causing
	 * OBS to receive a burst of frames. */
	if (clock->playout_anchor_valid) {
		if (pts_ns >= clock->playout_input_anchor_pts_ns)
			return clock->output_anchor_ns +
				(pts_ns - clock->playout_input_anchor_pts_ns);

		uint64_t delta = clock->playout_input_anchor_pts_ns - pts_ns;
		return clock->output_anchor_ns > delta
			? clock->output_anchor_ns - delta
			: 0;
	}

	if (!clock->video_anchor_valid) {
		clock->video_anchor_valid = true;
		clock->video_anchor_pts_ns = pts_ns;
		clock->video_anchor_system_ns = monotonic_ns() +
			(uint64_t)(clock->target_ms + STEADY_OUTPUT_LEAD_MS) * 1000000ULL;
	}
	if (pts_ns < clock->video_anchor_pts_ns)
		return clock->video_anchor_system_ns;
	return clock->video_anchor_system_ns + (pts_ns - clock->video_anchor_pts_ns);
}

static void output_audio_locked(steady_clock_t *clock, uint64_t now_ns)
{
	if (clock->audio_rate == 0 || clock->audio_channels == 0)
		return;

	const size_t output_frames = MAX(1, (size_t)((long double)clock->output_rate *
						STEADY_AUDIO_CHUNK_MS / 1000.0L));
	float speed = compute_speed_locked(clock, fill_ms_locked(clock));
	unsigned input_rate = (unsigned)llround(clock->output_rate * speed);
	input_rate = CLAMP(input_rate, (unsigned)llround(clock->output_rate * STEADY_SPEED_MIN),
				   (unsigned)llround(clock->output_rate * STEADY_SPEED_MAX));
	if (!ensure_resampler_locked(clock, clock->audio_channels, input_rate))
		return;

	size_t input_frames = gst_audio_resampler_get_in_frames(clock->resampler,
								 output_frames);
	if (input_frames == 0)
		input_frames = 1;
	if (!ensure_scratch(&clock->input_scratch, &clock->input_scratch_frames,
				    input_frames, clock->audio_channels) ||
	    !ensure_scratch(&clock->output_scratch, &clock->output_scratch_frames,
				    output_frames, clock->audio_channels) ||
	    !ensure_last_output_locked(clock, clock->audio_channels))
		return;

	uint64_t input_pts_ns = 0;
	size_t copied = copy_audio_locked(clock, clock->input_scratch, input_frames,
					  clock->audio_channels, &input_pts_ns);
	if (copied < input_frames)
		memset(clock->input_scratch + copied * clock->audio_channels, 0,
		       (input_frames - copied) * clock->audio_channels * sizeof(float));

	if (copied == 0) {
		clock->stats.audio_underruns++;
		if (clock->primed && clock->last_audio_push_system_ns != 0 &&
		    now_ns - clock->last_audio_push_system_ns >
			    STEADY_MAX_LAG_MS * 1000000ULL &&
		    !clock->stall_reanchored) {
			clock->stats.clock_reanchors++;
			clock->output_anchor_ns = now_ns +
				(uint64_t)STEADY_OUTPUT_LEAD_MS * 1000000ULL;
			clock->output_samples = 0;
			clock->next_audio_ns = clock->output_anchor_ns;
			clock->latest_input_audio_end_ns = 0;
			clock->latest_output_audio_end_ns = 0;
			clock->video_anchor_valid = false;
			clock->stall_reanchored = true;
		}
	}

	/* If a stall forced a new playout epoch, bind the first real audio sample
	 * to the first output timestamp of that epoch. This also makes the mapping
	 * robust if the source did not provide video before the first audio output.
	 */
	if (copied > 0 && !clock->playout_anchor_valid) {
		clock->playout_input_anchor_pts_ns = input_pts_ns;
		clock->playout_anchor_valid = true;
	}

	gpointer input_planes[1] = {clock->input_scratch};
	gpointer output_planes[1] = {clock->output_scratch};
	gst_audio_resampler_resample(clock->resampler, input_planes, input_frames,
					     output_planes, output_frames);

	if (copied == 0) {
		size_t fade_frames = MIN(output_frames,
			(size_t)MAX(1, (int)((long double)clock->output_rate *
							STEADY_SILENCE_FADE_MS / 1000.0L)));
		for (size_t i = 0; i < fade_frames; i++) {
			float gain = (float)(fade_frames - i - 1) / fade_frames;
			for (unsigned channel = 0; channel < clock->audio_channels; channel++)
				clock->output_scratch[i * clock->audio_channels + channel] =
					clock->last_output[channel] * gain;
		}
		if (fade_frames < output_frames)
			memset(clock->output_scratch + fade_frames * clock->audio_channels, 0,
			       (output_frames - fade_frames) * clock->audio_channels * sizeof(float));
	} else if (clock->last_output_was_silence) {
		size_t fade_frames = MIN(output_frames,
			(size_t)MAX(1, (int)((long double)clock->output_rate *
							STEADY_SILENCE_FADE_MS / 1000.0L)));
		for (size_t i = 0; i < fade_frames; i++) {
			float gain = (float)(i + 1) / fade_frames;
			for (unsigned channel = 0; channel < clock->audio_channels; channel++)
				clock->output_scratch[i * clock->audio_channels + channel] *= gain;
		}
	}
	memcpy(clock->last_output,
	       clock->output_scratch + (output_frames - 1) * clock->audio_channels,
	       clock->audio_channels * sizeof(float));
	clock->last_output_was_silence = copied == 0;

	uint64_t output_ts = clock->next_audio_ns;
	clock->next_audio_ns += frames_to_ns(output_frames, clock->output_rate);
	clock->output_samples += output_frames;
	clock->latest_output_audio_end_ns = clock->next_audio_ns;
	if (copied > 0)
		clock->latest_input_audio_end_ns = input_pts_ns +
			frames_to_ns(copied, clock->audio_rate);

	/* Do not call application code while holding the clock lock. Apart from
	 * reducing lock contention, this lets OBS-side callbacks inspect the
	 * statistics without creating a lock cycle. The scratch buffer remains
	 * owned by the clock until the synchronous callback returns. */
	void (*audio_callback)(void *, const float *, size_t, unsigned, unsigned,
				      uint64_t) = clock->callbacks.audio;
	void *opaque = clock->opaque;
	unsigned channels = clock->audio_channels;
	unsigned output_rate = clock->output_rate;
	float *samples = clock->output_scratch;
	g_mutex_unlock(&clock->mutex);
	audio_callback(opaque, samples, output_frames, channels, output_rate, output_ts);
	g_mutex_lock(&clock->mutex);
}

static void output_video_locked(steady_clock_t *clock, uint64_t now_ns)
{
	if (clock->next_video_ns > now_ns)
		return;

	while (!g_queue_is_empty(clock->video)) {
		video_frame_t *frame = g_queue_peek_head(clock->video);
		uint64_t due = video_due_ns_locked(clock, frame->pts_ns);
		if (due > now_ns)
			break;

		g_queue_pop_head(clock->video);
		if (now_ns > due + STEADY_VIDEO_LATE_TOLERANCE_NS) {
			clock->stats.late_video_frames++;
			free_video_frame(frame);
			continue;
		}
		void (*video_callback)(void *, GstSample *, uint64_t) = clock->callbacks.video;
		void *opaque = clock->opaque;
		GstSample *sample = frame->sample;
		g_mutex_unlock(&clock->mutex);
		video_callback(opaque, sample, due);
		g_mutex_lock(&clock->mutex);
		uint64_t interval = frame->duration_ns;
		if (interval == 0 || interval > 250000000ULL)
			interval = STEADY_VIDEO_INTERVAL_NS;
		clock->next_video_ns = due + interval;
		free_video_frame(frame);
		/* Never drain multiple video frames in one scheduler pass. If several
		 * frames are already due, the next pass will either wait for the frame
		 * interval or discard the stale head. */
		break;
	}
}

static gpointer steady_clock_thread(gpointer user_data)
{
	steady_clock_t *clock = user_data;

	g_mutex_lock(&clock->mutex);
	while (!clock->stop_requested) {
		uint64_t now = monotonic_ns();
		int fill_ms = fill_ms_locked(clock);

		if (!clock->primed) {
			if (fill_ms < clock->target_ms + STEADY_OUTPUT_LEAD_MS) {
				g_cond_wait_until(&clock->cond, &clock->mutex,
							g_get_monotonic_time() + 10000);
				continue;
			}
			clock->primed = true;
			clock->output_anchor_ns = now +
				(uint64_t)STEADY_OUTPUT_LEAD_MS * 1000000ULL;
			audio_chunk_t *first_audio = g_queue_peek_head(clock->audio);
			if (first_audio) {
				clock->playout_input_anchor_pts_ns = first_audio->pts_ns +
					frames_to_ns(first_audio->offset, first_audio->sample_rate);
				clock->playout_anchor_valid = true;
			}
			clock->next_audio_ns = clock->output_anchor_ns;
			clock->next_video_ns = 0;
			clock->output_samples = 0;
			clock->output_speed = 1.0f;
			clock->stats.primed = true;
		}

		uint64_t next_wake = clock->next_audio_ns;
		if (!g_queue_is_empty(clock->video)) {
			video_frame_t *frame = g_queue_peek_head(clock->video);
			uint64_t due = video_due_ns_locked(clock, frame->pts_ns);
			if (clock->next_video_ns > now && clock->next_video_ns < next_wake)
				next_wake = clock->next_video_ns;
			else if (clock->next_video_ns <= now && due < next_wake)
				next_wake = due;
		}

		if (next_wake > now) {
			g_cond_wait_until(&clock->cond, &clock->mutex,
						(gint64)(next_wake / 1000ULL));
			continue;
		}

		if (clock->next_audio_ns <= now)
			output_audio_locked(clock, now);
		output_video_locked(clock, now);
	}
	g_mutex_unlock(&clock->mutex);
	return NULL;
}

steady_clock_t *steady_clock_create(void *opaque,
				    const struct steady_clock_callbacks *callbacks,
				    unsigned output_rate, int target_ms,
				    bool adaptive_speed)
{
	if (!callbacks || !callbacks->audio || !callbacks->video || output_rate == 0)
		return NULL;

	steady_clock_t *clock = g_new0(steady_clock_t, 1);
	clock->opaque = opaque;
	clock->callbacks = *callbacks;
	clock->output_rate = output_rate;
	clock->target_ms = CLAMP(target_ms, 50, 1000);
	clock->min_ms = MAX(STEADY_MIN_BUFFER_MS, clock->target_ms / 2);
	clock->max_ms = clock->target_ms + STEADY_MAX_BUFFER_EXTRA_MS;
	clock->adaptive_speed = adaptive_speed;
	clock->output_speed = 1.0f;
	clock->audio = g_queue_new();
	clock->video = g_queue_new();
	g_mutex_init(&clock->mutex);
	g_cond_init(&clock->cond);
	return clock;
}

void steady_clock_destroy(steady_clock_t *clock)
{
	if (!clock)
		return;
	steady_clock_stop(clock);
	g_mutex_lock(&clock->mutex);
	clear_queues_locked(clock);
	reset_resampler_locked(clock);
	g_mutex_unlock(&clock->mutex);
	g_queue_free(clock->audio);
	g_queue_free(clock->video);
	g_free(clock->input_scratch);
	g_free(clock->output_scratch);
	g_free(clock->last_output);
	g_cond_clear(&clock->cond);
	g_mutex_clear(&clock->mutex);
	g_free(clock);
}

void steady_clock_start(steady_clock_t *clock)
{
	if (!clock)
		return;
	g_mutex_lock(&clock->mutex);
	if (!clock->started) {
		clock->stop_requested = false;
		clock->started = true;
		clock->thread = g_thread_new("GStreamer steady clock",
					    steady_clock_thread, clock);
	}
	g_mutex_unlock(&clock->mutex);
}

void steady_clock_stop(steady_clock_t *clock)
{
	if (!clock)
		return;
	g_mutex_lock(&clock->mutex);
	if (!clock->started) {
		g_mutex_unlock(&clock->mutex);
		return;
	}
	clock->stop_requested = true;
	g_cond_broadcast(&clock->cond);
	GThread *thread = clock->thread;
	clock->thread = NULL;
	clock->started = false;
	g_mutex_unlock(&clock->mutex);
	if (thread)
		g_thread_join(thread);
}

void steady_clock_reset(steady_clock_t *clock)
{
	if (!clock)
		return;
	g_mutex_lock(&clock->mutex);
	clear_queues_locked(clock);
	reset_resampler_locked(clock);
	clock->primed = false;
	clock->stats.primed = false;
	clock->output_anchor_ns = 0;
	clock->output_samples = 0;
	clock->next_audio_ns = 0;
	clock->latest_input_audio_end_ns = 0;
	clock->latest_output_audio_end_ns = 0;
	clock->last_input_pts_ns = 0;
	clock->input_anchor_system_ns = 0;
	clock->input_anchor_pts_ns = 0;
	clock->last_audio_push_system_ns = 0;
	clock->input_anchor_valid = false;
	clock->stall_reanchored = false;
	clock->video_anchor_valid = false;
	clock->playout_anchor_valid = false;
	clock->playout_input_anchor_pts_ns = 0;
	clock->next_video_ns = 0;
	clock->output_speed = 1.0f;
	if (clock->last_output)
		memset(clock->last_output, 0,
		       clock->last_output_channels * sizeof(float));
	clock->last_output_was_silence = false;
	g_cond_broadcast(&clock->cond);
	g_mutex_unlock(&clock->mutex);
}

bool steady_clock_push_audio(steady_clock_t *clock, const float *samples,
				     size_t frames, unsigned channels,
				     unsigned sample_rate, uint64_t pts_ns)
{
	if (!clock || !samples || frames == 0 || channels == 0 || sample_rate == 0)
		return false;

	audio_chunk_t *chunk = g_new0(audio_chunk_t, 1);
	chunk->samples = g_malloc_n(frames * channels, sizeof(float));
	chunk->frames = frames;
	chunk->channels = channels;
	chunk->sample_rate = sample_rate;
	chunk->pts_ns = pts_ns;
	if (!chunk->samples) {
		free_audio_chunk(chunk);
		return false;
	}
	memcpy(chunk->samples, samples, frames * channels * sizeof(float));

	g_mutex_lock(&clock->mutex);
	if (clock->audio_rate != 0 && clock->audio_rate != sample_rate) {
		clear_queues_locked(clock);
		reset_resampler_locked(clock);
		clock->primed = false;
		clock->input_anchor_valid = false;
	}
	clock->audio_rate = sample_rate;
	clock->audio_channels = channels;
	uint64_t now_ns = monotonic_ns();

	if (clock->last_input_pts_ns != 0 &&
	    (pts_ns < clock->last_input_pts_ns ||
	     pts_ns - clock->last_input_pts_ns > STEADY_PTS_RESET_MS * 1000000ULL)) {
		clear_queues_locked(clock);
		reset_resampler_locked(clock);
		clock->primed = false;
		clock->video_anchor_valid = false;
		clock->playout_anchor_valid = false;
		clock->next_video_ns = 0;
		clock->input_anchor_valid = false;
	}
	if (!clock->input_anchor_valid) {
		clock->input_anchor_system_ns = now_ns;
		clock->input_anchor_pts_ns = pts_ns;
		clock->input_anchor_valid = true;
	}
	clock->last_audio_push_system_ns = now_ns;
	clock->stall_reanchored = false;
	clock->last_input_pts_ns = pts_ns + frames_to_ns(frames, sample_rate);
	g_queue_push_tail(clock->audio, chunk);
	clock->audio_frames += frames;

	/* Keep a generous bounded reserve. Under normal conditions the
	 * adaptive rate controller drains the excess. Only an extreme sustained
	 * producer overrun reaches this emergency trim. */
	size_t cap_frames = (size_t)((long double)clock->output_rate *
			(clock->max_ms * STEADY_BUFFER_CAP_MULTIPLIER) / 1000.0L);
	while (clock->audio_frames > cap_frames && !g_queue_is_empty(clock->audio)) {
		free_audio_chunk(g_queue_pop_head(clock->audio));
		clock->audio_frames = 0;
		for (GList *item = clock->audio->head; item; item = item->next)
			clock->audio_frames += ((audio_chunk_t *)item->data)->frames -
				((audio_chunk_t *)item->data)->offset;
	}
	g_cond_signal(&clock->cond);
	g_mutex_unlock(&clock->mutex);
	return true;
}

bool steady_clock_push_video(steady_clock_t *clock, GstSample *sample,
				     uint64_t pts_ns, uint64_t duration_ns)
{
	if (!clock || !sample || pts_ns == GST_CLOCK_TIME_NONE)
		return false;
	video_frame_t *frame = g_new0(video_frame_t, 1);
	frame->sample = gst_sample_ref(sample);
	frame->pts_ns = pts_ns;
	frame->duration_ns = duration_ns;
	g_mutex_lock(&clock->mutex);
	g_queue_push_tail(clock->video, frame);
	while (g_queue_get_length(clock->video) > STEADY_MAX_VIDEO_QUEUE) {
		clock->stats.late_video_frames++;
		free_video_frame(g_queue_pop_head(clock->video));
	}
	g_cond_signal(&clock->cond);
	g_mutex_unlock(&clock->mutex);
	return true;
}

void steady_clock_get_stats(steady_clock_t *clock,
				    struct steady_clock_stats *stats)
{
	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!clock)
		return;
	g_mutex_lock(&clock->mutex);
	*stats = clock->stats;
	stats->buffer_fill_ms = fill_ms_locked(clock);
	if (clock->input_anchor_valid && clock->latest_input_audio_end_ns != 0) {
		uint64_t now = monotonic_ns();
		uint64_t source_due = clock->input_anchor_system_ns +
			(clock->latest_input_audio_end_ns - clock->input_anchor_pts_ns);
		stats->stream_delay_ms = now > source_due
			? (int)((now - source_due) / 1000000ULL)
			: 0;
	}
	stats->output_speed = clock->output_speed;
	g_mutex_unlock(&clock->mutex);
}
