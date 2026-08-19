#include "../steady-clock.h"

#include <glib.h>

#include <math.h>

struct capture {
	GMutex mutex;
	GArray *timestamps;
	GArray *video_timestamps;
	unsigned rate;
	unsigned channels;
	unsigned nonzero_callbacks;
};

static void capture_audio(void *opaque, const float *samples, size_t frames,
				  unsigned channels, unsigned rate,
				  uint64_t timestamp_ns)
{
	struct capture *capture = opaque;
	bool nonzero = false;
	for (size_t i = 0; i < frames * channels; i++) {
		if (fabsf(samples[i]) > 0.0001f) {
			nonzero = true;
			break;
		}
	}

	g_mutex_lock(&capture->mutex);
	g_array_append_val(capture->timestamps, timestamp_ns);
	capture->rate = rate;
	capture->channels = channels;
	if (nonzero)
		capture->nonzero_callbacks++;
	g_mutex_unlock(&capture->mutex);
}

static void capture_video(void *opaque, GstSample *sample,
				  uint64_t timestamp_ns)
{
	struct capture *capture = opaque;
	(void)sample;
	g_mutex_lock(&capture->mutex);
	g_array_append_val(capture->video_timestamps, timestamp_ns);
	g_mutex_unlock(&capture->mutex);
}

static void push_audio(steady_clock_t *clock, float value, uint64_t pts_ns)
{
	float samples[960 * 2];
	for (size_t i = 0; i < G_N_ELEMENTS(samples); i++)
		samples[i] = value;
	g_assert_true(steady_clock_push_audio(clock, samples, 960, 2, 48000, pts_ns));
}

static void test_fixed_audio_clock(void)
{
	struct capture capture = {0};
	g_mutex_init(&capture.mutex);
	capture.timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	capture.video_timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	struct steady_clock_callbacks callbacks = {
		.audio = capture_audio,
		.video = capture_video,
	};
	steady_clock_t *clock = steady_clock_create(&capture, &callbacks, 48000,
							120, true);
	g_assert_nonnull(clock);
	steady_clock_start(clock);

	for (unsigned i = 0; i < 16; i++)
		push_audio(clock, 0.25f, (uint64_t)i * 20000000ULL);

	g_usleep(450000);
	struct steady_clock_stats stats = {0};
	steady_clock_get_stats(clock, &stats);
	g_assert_true(stats.primed);
	g_assert_cmpuint(capture.timestamps->len, >, 3);
	g_assert_cmpuint(capture.nonzero_callbacks, >, 1);

	for (guint i = 1; i < capture.timestamps->len; i++) {
		uint64_t previous = g_array_index(capture.timestamps, uint64_t, i - 1);
		uint64_t current = g_array_index(capture.timestamps, uint64_t, i);
		g_assert_cmpuint(current, >, previous);
		g_assert_cmpuint(current - previous, >=, 19000000ULL);
		g_assert_cmpuint(current - previous, <=, 21000000ULL);
	}

	steady_clock_reset(clock);
	steady_clock_get_stats(clock, &stats);
	g_assert_false(stats.primed);
	steady_clock_destroy(clock);
	g_array_free(capture.timestamps, TRUE);
	g_array_free(capture.video_timestamps, TRUE);
	g_mutex_clear(&capture.mutex);
}

static void test_adaptive_backlog_bounds(void)
{
	struct capture capture = {0};
	g_mutex_init(&capture.mutex);
	capture.timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	capture.video_timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	struct steady_clock_callbacks callbacks = {
		.audio = capture_audio,
		.video = capture_video,
	};
	steady_clock_t *clock = steady_clock_create(&capture, &callbacks, 48000,
							120, true);
	steady_clock_start(clock);

	for (unsigned i = 0; i < 100; i++)
		push_audio(clock, 0.1f, (uint64_t)i * 20000000ULL);
	g_usleep(250000);

	struct steady_clock_stats stats = {0};
	steady_clock_get_stats(clock, &stats);
	g_assert_cmpfloat(stats.output_speed, >=, 0.98f);
	g_assert_cmpfloat(stats.output_speed, <=, 1.05f);
	g_assert_cmpint(stats.buffer_fill_ms, >=, 0);

	steady_clock_destroy(clock);
	g_array_free(capture.timestamps, TRUE);
	g_array_free(capture.video_timestamps, TRUE);
	g_mutex_clear(&capture.mutex);
}

static GstSample *make_test_video_sample(void)
{
	GstCaps *caps = gst_caps_from_string(
		"video/x-raw,format=I420,width=2,height=2,framerate=30/1");
	GstBuffer *buffer = gst_buffer_new_allocate(NULL, 6, NULL);
	return gst_sample_new(buffer, caps, NULL, NULL);
}

static void test_video_follows_audio_clock(void)
{
	struct capture capture = {0};
	g_mutex_init(&capture.mutex);
	capture.timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	capture.video_timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	struct steady_clock_callbacks callbacks = {
		.audio = capture_audio,
		.video = capture_video,
	};
	steady_clock_t *clock = steady_clock_create(&capture, &callbacks, 48000,
							50, false);
	g_assert_nonnull(clock);
	steady_clock_start(clock);
	for (unsigned i = 0; i < 8; i++)
		push_audio(clock, 0.25f, (uint64_t)i * 20000000ULL);
	GstSample *sample = make_test_video_sample();
	g_assert_true(steady_clock_push_video(clock, sample, 0, 33333333));
	gst_sample_unref(sample);

	g_usleep(350000);
	g_assert_cmpuint(capture.timestamps->len, >, 0);
	g_assert_cmpuint(capture.video_timestamps->len, ==, 1);
	uint64_t audio_ts = g_array_index(capture.timestamps, uint64_t, 0);
	uint64_t video_ts = g_array_index(capture.video_timestamps, uint64_t, 0);
	g_assert_cmpuint(audio_ts > video_ts ? audio_ts - video_ts : video_ts - audio_ts,
				<, 5000000ULL);

	steady_clock_destroy(clock);
	g_array_free(capture.timestamps, TRUE);
	g_array_free(capture.video_timestamps, TRUE);
	g_mutex_clear(&capture.mutex);
}

static void test_underrun_reanchors_once(void)
{
	struct capture capture = {0};
	g_mutex_init(&capture.mutex);
	capture.timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	capture.video_timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	struct steady_clock_callbacks callbacks = {
		.audio = capture_audio,
		.video = capture_video,
	};
	steady_clock_t *clock = steady_clock_create(&capture, &callbacks, 48000,
							50, true);
	g_assert_nonnull(clock);
	steady_clock_start(clock);
	for (unsigned i = 0; i < 8; i++)
		push_audio(clock, 0.25f, (uint64_t)i * 20000000ULL);
	g_usleep(1750000);

	struct steady_clock_stats stats = {0};
	steady_clock_get_stats(clock, &stats);
	g_assert_cmpint(stats.audio_underruns, >, 0);
	g_assert_cmpint(stats.clock_reanchors, ==, 1);
	for (guint i = 1; i < capture.timestamps->len; i++) {
		uint64_t previous = g_array_index(capture.timestamps, uint64_t, i - 1);
		uint64_t current = g_array_index(capture.timestamps, uint64_t, i);
		/* A stall must not make the timestamp handed to OBS jump forward. */
		g_assert_cmpuint(current - previous, >=, 19000000ULL);
		g_assert_cmpuint(current - previous, <=, 21000000ULL);
	}

	steady_clock_destroy(clock);
	g_array_free(capture.timestamps, TRUE);
	g_array_free(capture.video_timestamps, TRUE);
	g_mutex_clear(&capture.mutex);
}

static void test_discontinuous_audio_reprime(void)
{
	struct capture capture = {0};
	g_mutex_init(&capture.mutex);
	capture.timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	capture.video_timestamps = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	struct steady_clock_callbacks callbacks = {
		.audio = capture_audio,
		.video = capture_video,
	};
	steady_clock_t *clock = steady_clock_create(&capture, &callbacks, 48000,
							50, true);
	g_assert_nonnull(clock);
	steady_clock_start(clock);
	for (unsigned i = 0; i < 8; i++)
		push_audio(clock, 0.25f, (uint64_t)i * 20000000ULL);
	g_usleep(300000);
	for (unsigned i = 0; i < 8; i++)
		push_audio(clock, 0.25f, 10000000000ULL + (uint64_t)i * 20000000ULL);
	g_usleep(350000);

	struct steady_clock_stats stats = {0};
	steady_clock_get_stats(clock, &stats);
	g_assert_true(stats.primed);
	g_assert_cmpuint(capture.nonzero_callbacks, >, 2);

	steady_clock_destroy(clock);
	g_array_free(capture.timestamps, TRUE);
	g_array_free(capture.video_timestamps, TRUE);
	g_mutex_clear(&capture.mutex);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	gst_init(&argc, &argv);
	g_test_add_func("/steady-clock/fixed-audio-clock", test_fixed_audio_clock);
	g_test_add_func("/steady-clock/adaptive-backlog-bounds", test_adaptive_backlog_bounds);
	g_test_add_func("/steady-clock/video-follows-audio-clock", test_video_follows_audio_clock);
	g_test_add_func("/steady-clock/underrun-reanchors-once", test_underrun_reanchors_once);
	g_test_add_func("/steady-clock/discontinuous-audio-reprime", test_discontinuous_audio_reprime);
	return g_test_run();
}
