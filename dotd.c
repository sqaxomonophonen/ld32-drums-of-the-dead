#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "platform.h"

#include "a.h"

// SONGS
#include "song.h"
#include "song.xrns.inc.c"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "stb_vorbis.c"

#include <SDL.h>

#ifndef M_PI
#define M_PI (3.141592653589793)
#endif

// 16:9
#define SCREEN_WIDTH (384)
#define SCREEN_HEIGHT (216)

// your random number god
struct rng {
	uint32_t z;
	uint32_t w;
};

static inline uint32_t rng_uint32(struct rng* rng)
{
	/*
	   The MWC generator concatenates two 16-bit multiply-
	   with-carry generators, x(n)=36969x(n-1)+carry,
	   y(n)=18000y(n-1)+carry mod 2^16, has period about
	   2^60 and seems to pass all tests of randomness. A
	   favorite stand-alone generator---faster than KISS,
	   which contains it.
	*/
	rng->z = 36969 * (rng->z & 65535) + (rng->z>>16);
	rng->w = 18000 * (rng->w & 65535) + (rng->w>>16);
	return (rng->z<<16) + rng->w;
}

static inline float rng_float(struct rng* rng)
{
	union {
		uint32_t i;
		float f;
	} magick;
	uint32_t r = rng_uint32(rng);
	magick.i = (r & 0x007fffff) | (127 << 23);
	return magick.f - 1;
}

static inline void rng_seed(struct rng* rng, uint32_t seed)
{
	rng->z = 654654 + seed;
	rng->w = 7653234 + seed * 69069;
}




#define ASSET_PATH_MAX_LENGTH (1500)
static char assets_base[ASSET_PATH_MAX_LENGTH];

static char _assets_tmp[ASSET_PATH_MAX_LENGTH + 256];
static char* asset_path(const char* asset)
{
	strcpy(_assets_tmp, assets_base);
	#if BUILD_MINGW32
	const char* postfix = "assets\\";
	#else
	const char* postfix = "assets/";
	#endif
	strcpy(_assets_tmp + strlen(_assets_tmp), postfix);
	strcpy(_assets_tmp + strlen(_assets_tmp), asset);
	return _assets_tmp;
}





struct sample { // always stereo
	float* data;
	uint32_t length;
};

static void sample_load(struct sample* sample, const char* asset)
{
	SDL_AudioSpec want;
	want.freq = 44100;
	want.format = AUDIO_S16;
	want.channels = 2;

	uint8_t* data;
	uint32_t length;

	SDL_AudioSpec* got = SDL_LoadWAV(asset_path(asset), &want, &data, &length);
	SAN(got);
	ASSERT(got->freq == 44100);
	ASSERT(got->channels == 2);
	ASSERT(got->format == AUDIO_S16);

	sample->length = (length / sizeof(int16_t)) >> 1;

	// convert
	int16_t* data16 = (int16_t*)data;
	sample->data = malloc(sizeof(float) * sample->length * 2);
	AN(sample->data);
	for (int i = 0; i < (sample->length << 1); i++) {
		sample->data[i] = (float)data16[i] / 32767.0f;
	}

	SDL_FreeWAV(data);

}

struct drum_samples {
	struct sample samples[4*3];
};


static void drum_samples_init(struct drum_samples* ds)
{
	const char* files[] = {
		"k0.wav",
		"k1.wav",
		"k2.wav",
		"s0.wav",
		"s1.wav",
		"s2.wav",
		"h0.wav",
		"h1.wav",
		"h2.wav",
		"o0.wav",
		"o1.wav",
		"o2.wav",
		NULL
	};

	const char** file = files;

	int i = 0;
	while (*file) {
		struct sample* sample = &ds->samples[i];
		sample_load(sample, *file);
		file++;
		i++;
	}
}


#define DRUM_ID_KICK (0)
#define DRUM_ID_SNARE (1)
#define DRUM_ID_HIHAT (2)
#define DRUM_ID_OPEN (3)
#define DRUM_ID_MAX (4)

#define DRUM_CONTROL_KICK (1<<(DRUM_ID_KICK))
#define DRUM_CONTROL_SNARE (1<<(DRUM_ID_SNARE))
#define DRUM_CONTROL_HIHAT (1<<(DRUM_ID_HIHAT))
#define DRUM_CONTROL_OPEN (1<<(DRUM_ID_OPEN))
#define DRUM_CONTROL_HEAD (1<<16)

struct drum_control_feedback {
	uint32_t value;
	uint32_t position;
};

#define DRUM_CONTROL_FEEDBACK_N (1<<8)
#define DRUM_CONTROL_FEEDBACK_MASK (DRUM_CONTROL_FEEDBACK_N-1)


struct sample_ctx {
	struct sample* sample;
	int position;
	int playing;
	// float rate; +/- 1.0? TODO
};

#define DRUM_CONTROL_RING_LENGTH (32)
struct audio {
	SDL_AudioDeviceID device;
	uint32_t sample_rate;
	struct rng rng;

	SDL_mutex* mutex;

	struct drum_samples drum_samples;

	stb_vorbis* bass_track;
	stb_vorbis* guitar_track;
	float* bass_buffer;
	float* guitar_buffer;

	// state
	int bass_stopped;
	int guitar_stopped;
	struct sample_ctx drum_sample_ctx[DRUM_ID_MAX];
	uint32_t position;
	int drum_control_feedback_cursor;
	struct drum_control_feedback drum_control_feedback[DRUM_CONTROL_FEEDBACK_N];
	uint32_t drum_control_ring[DRUM_CONTROL_RING_LENGTH];
	uint32_t drum_control_write_cursor;
	uint32_t drum_control_read_cursor;
};

static void audio_lock(struct audio* audio)
{
	SAZ(SDL_LockMutex(audio->mutex));
}

static void audio_unlock(struct audio* audio)
{
	SDL_UnlockMutex(audio->mutex);
}

// XXX audio lock assumed!
static void audio_emit_drum_control(struct audio* audio, uint32_t drum_control)
{
	if (!drum_control) return;
	audio->drum_control_ring[audio->drum_control_write_cursor] = drum_control;
	audio->drum_control_write_cursor = (audio->drum_control_write_cursor + 1) & (DRUM_CONTROL_RING_LENGTH-1);
}

static void audio_callback(void* userdata, Uint8* stream_u8, int bytes)
{
	struct audio* audio = userdata;
	float* stream = (float*)stream_u8;
	int n = bytes / sizeof(float) / 2;

	audio_lock(audio);
	uint32_t drum_control = 0;
	//printf("%d %d\n", audio->drum_control_read_cursor, audio->drum_control_write_cursor);
	while (audio->drum_control_read_cursor != audio->drum_control_write_cursor) {
		drum_control |= audio->drum_control_ring[audio->drum_control_read_cursor];
		//printf("read %d\n", drum_control);
		audio->drum_control_read_cursor = (audio->drum_control_read_cursor + 1) & (DRUM_CONTROL_RING_LENGTH-1);
	}
	audio_unlock(audio);

	// trigger drums
	for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
		int m = 1<<drum_id;
		if (drum_control & m) {
			struct sample_ctx* ctx = &audio->drum_sample_ctx[drum_id];
			int di = drum_id*3 + rng_uint32(&audio->rng) % 3;
			ctx->sample = &audio->drum_samples.samples[di];
			ctx->position = 0;
			ctx->playing = 1;

			// open/close hihack
			if (drum_id == DRUM_ID_HIHAT) {
				audio->drum_sample_ctx[DRUM_ID_OPEN].playing = 0;
			}
		}
	}

	int na;
	if (!audio->bass_stopped) {
		na = stb_vorbis_get_samples_float_interleaved(audio->bass_track, 2, audio->bass_buffer, bytes / (sizeof(float)));
		if (na == 0) audio->bass_stopped = 1;
	}
	
	if (!audio->guitar_stopped) {
		na = stb_vorbis_get_samples_float_interleaved(audio->guitar_track, 2, audio->guitar_buffer, bytes / (sizeof(float)));
		if (na == 0) audio->guitar_stopped = 1;
	}

	// generate samples
	int p = 0;
	for (int i = 0; i < n; i++) {
		float out[2] = {0,0};

		for (int c = 0; c < 2; c++) {
			if (!audio->bass_stopped) out[c] += audio->bass_buffer[(i<<1)+c];
			if (!audio->guitar_stopped) out[c] += audio->guitar_buffer[(i<<1)+c];
		}

		// generate drum audio
		for (int j = 0; j < DRUM_ID_MAX; j++) {
			struct sample_ctx* ctx = &audio->drum_sample_ctx[j];
			if (!ctx->playing) continue;
			for (int c = 0; c < 2; c++) {
				out[c] += ctx->sample->data[(ctx->position << 1) + c];
			}
			ctx->position++;
			if (ctx->position >= ctx->sample->length) {
				ctx->playing = 0;
			}
		}


		for (int c = 0; c < 2; c++) stream[p++] = out[c];
	}


	audio_lock(audio);
	{
		// advance drum control feedback, register what happened and when
		audio->drum_control_feedback_cursor = (audio->drum_control_feedback_cursor + 1) & DRUM_CONTROL_FEEDBACK_MASK;
		struct drum_control_feedback* f = &audio->drum_control_feedback[audio->drum_control_feedback_cursor];
		f->position = audio->position;
		f->value = drum_control;
	}
	audio_unlock(audio);

	audio->position += n;

}

static void audio_start(struct audio* audio, int audio_buffer_length_exp)
{
	audio->bass_stopped = 0;
	audio->guitar_stopped = 0;
	memset(audio->drum_sample_ctx, 0, sizeof(struct sample_ctx) * DRUM_ID_MAX);
	audio->position = 0;
	audio->drum_control_feedback_cursor = 0;
	memset(audio->drum_control_feedback, 0, sizeof(struct drum_control_feedback) * DRUM_CONTROL_FEEDBACK_N);
	memset(audio->drum_control_ring, 0, sizeof(uint32_t) * DRUM_CONTROL_RING_LENGTH);
	audio->drum_control_write_cursor = 0;
	audio->drum_control_read_cursor = 0;

	#if 0
	int bass_stopped;
	int guitar_stopped;
	struct sample_ctx drum_sample_ctx[DRUM_ID_MAX];
	uint32_t position;
	int drum_control_feedback_cursor;
	struct drum_control_feedback drum_control_feedback[DRUM_CONTROL_FEEDBACK_N];
	uint32_t drum_control_ring[DRUM_CONTROL_RING_LENGTH];
	uint32_t drum_control_write_cursor;
	uint32_t drum_control_read_cursor;
	#endif

	stb_vorbis_seek_start(audio->bass_track);
	stb_vorbis_seek_start(audio->guitar_track);

	SDL_AudioSpec want, have;
	want.freq = 44100;
	want.format = AUDIO_F32;
	want.channels = 2;
	want.samples = 256 << audio_buffer_length_exp;
	// XXX ^^^ might want to be able to change this..
	// 16th notes at 120BPM is 0.125s between notes
	// 512 samples per callback at 44100hz is ~0.012s per frame, i.e. ~10x
	// more accurate
	// 512 samples per callback annoys wine, 1024 works fine (~5x more
	// accurate)
	// 1024 *FEELS* sligtly sluggish with fast notes
	// 512 feels fine, 256 feels better
	want.callback = audio_callback;
	want.userdata = audio;

	audio->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio->device == 0) arghf("SDL_OpenAudioDevice: %s", SDL_GetError());

	audio->sample_rate = have.freq;

	if (audio->bass_buffer) free(audio->bass_buffer);
	audio->bass_buffer = malloc(sizeof(float) * 2 * have.samples);
	AN(audio->bass_buffer);

	if (audio->guitar_buffer) free(audio->guitar_buffer);
	audio->guitar_buffer = malloc(sizeof(float) * 2 * have.samples);
	AN(audio->guitar_buffer);

	SDL_PauseAudioDevice(audio->device, 0);
}

static void audio_stop(struct audio* audio)
{
	SDL_PauseAudioDevice(audio->device, 1);
	SDL_CloseAudioDevice(audio->device);
}

static void audio_init(struct audio* audio)
{
	memset(audio, 0, sizeof(*audio));

	rng_seed(&audio->rng, 0);

	int vorbis_error;

	audio->bass_track = stb_vorbis_open_filename(asset_path("basstrack.ogg"), &vorbis_error, NULL);
	if (audio->bass_track == NULL) arghf("stb_vorbis_open_filename() failed for basstrack (%d)", vorbis_error);

	audio->guitar_track = stb_vorbis_open_filename(asset_path("guitartrack.ogg"), &vorbis_error, NULL);
	if (audio->guitar_track == NULL) arghf("stb_vorbis_open_filename() failed for guitartrack (%d)", vorbis_error);

	audio->mutex = SDL_CreateMutex();
	SAN(audio->mutex);

	drum_samples_init(&audio->drum_samples);

}

static void audio_quit(struct audio* audio)
{
	SDL_DestroyMutex(audio->mutex);
	SDL_CloseAudioDevice(audio->device);
}

static float audio_position_to_seconds(struct audio* audio, uint32_t position)
{
	return (float)position / (float)audio->sample_rate;
}


#define MAX_PLAYED_NOTES (256)

struct played_note {
	float time_in_seconds;
	uint32_t drum_id;
};

struct piano_roll {
	struct song* song;

	// state
	float time_in_seconds;
	struct played_note* played_notes;
	float gauge;
	int gauge_last_step;
};

static void piano_roll_init(struct piano_roll* p, struct song* song)
{
	memset(p, 0, sizeof(*p));
	p->song = song;
	p->played_notes = calloc(MAX_PLAYED_NOTES, sizeof(*p->played_notes));
	AN(p->played_notes);
}

static void piano_roll_reset(struct piano_roll* p)
{
	p->time_in_seconds = 0.0f;
	p->gauge = 1.0f;
	p->gauge_last_step = 0;
	memset(p->played_notes, 0, sizeof(*p->played_notes) * MAX_PLAYED_NOTES);
}

static void piano_roll_update_position(struct piano_roll* p, struct audio* audio, uint32_t audio_position)
{
	p->time_in_seconds = audio_position_to_seconds(audio, audio_position);
}

static void piano_roll_gauge_dstep(struct piano_roll* p, int match, float dstep)
{
	float dpenalty = match ? (dstep / (float)p->song->lpb) : 1.0f;
	if (dpenalty > 1.0f) dpenalty = 1.0f;
	float good_threshold = 0.29f;
	p->gauge -= ((dpenalty * dpenalty) - (good_threshold * good_threshold)) * 0.1f;
}

static void piano_roll_update_gauge(struct piano_roll* p)
{
	// TODO bonus after 4 bars or something, or at certain points in the
	// music .. export from .xrns?
	float current_step = (p->time_in_seconds * (float)p->song->bpm * (float)p->song->lpb) / 60.0;
	int step_until = floorf(current_step - (float)p->song->lpb);
	for (int step = p->gauge_last_step; step < step_until; step++) {
		if (step < 0 || step >= p->song->length) continue;
		int d = p->song->drums[step];
		if (d == 0) continue;

		float step_time = (((float)step / (float)p->song->lpb) / (float)p->song->bpm) * 60.0;
		
		int match = 0;
		float dt_min;
		for (int i = 0; i < MAX_PLAYED_NOTES; i++) {
			struct played_note* note = &p->played_notes[i];
			if ((d & (1<<note->drum_id)) == 0) continue;
			float dt = fabsf(note->time_in_seconds - step_time);
			if (!match || dt < dt_min) {
				match = 1;
				dt_min = dt;
			}
		}
		float dstep_min = (dt_min * (float)p->song->bpm * (float)p->song->lpb) / 60.0;
		piano_roll_gauge_dstep(p, match, dstep_min);
	}
	p->gauge_last_step = step_until;

	if (p->gauge > 1.0) p->gauge = 1.0;
	if (p->gauge < 0.0) p->gauge = 0.0;
}

static void piano_roll_gauge_play(struct piano_roll* p, struct played_note* note)
{
	p->gauge -= 0.01f; // penalty for playing; to prevent spamming abuse

	float note_step = (note->time_in_seconds * (float)p->song->bpm * (float)p->song->lpb) / 60.0;
	float range = p->song->lpb;
	int step_min = (int)floorf(note_step - range);
	int step_max = (int)ceilf(note_step + range);

	int match = 0;
	float dstep_min = 0;
	for (int step = step_min; step <= step_max; step++) {
		if (step < 0 || step >= p->song->length) continue;
		int d = p->song->drums[step];
		if ((d & (1<<note->drum_id)) == 0) continue;
		float dstep = fabsf(note_step - (float)step);
		if (!match || dstep < dstep_min) {
			match = 1;
			dstep_min = dstep;
		}
	}

	piano_roll_gauge_dstep(p, match, dstep_min);
}

static void piano_roll_register_drum_control_feedback(struct piano_roll* p, struct audio* audio, struct drum_control_feedback* fb)
{
	for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
		int drum_id_mask = 1<<drum_id;
		if (!(fb->value & drum_id_mask)) continue;
		float earliest_time = 0;
		int earliest_time_idx = -1;
		for (int j = 0; j < MAX_PLAYED_NOTES; j++) {
			struct played_note* note = &p->played_notes[j];
			if (earliest_time_idx == -1 || note->time_in_seconds < earliest_time) {
				earliest_time_idx = j;
				earliest_time = note->time_in_seconds;
			}
		}
		ASSERT(earliest_time_idx != -1);
		ASSERT(earliest_time_idx < MAX_PLAYED_NOTES);
		struct played_note* note = &p->played_notes[earliest_time_idx];
		note->drum_id = drum_id;
		note->time_in_seconds = audio_position_to_seconds(audio, fb->position);
		piano_roll_gauge_play(p, note);
	}
}

static uint32_t mkcol(int r, int g, int b)
{
	return (r&255) + ((g&255)<<8) + ((b&255)<<16);
}

static int screen_clip_rect(int* x0, int* y0, int* w, int* h, int* xmod, int* ymod)
{
	AN(x0);
	AN(y0);
	AN(w);
	AN(h);

	if (*x0 >= SCREEN_WIDTH) return 0;
	if (*y0 >= SCREEN_HEIGHT) return 0;

	if (*x0 < 0) {
		*w += *x0;
		if (xmod) *xmod -= *x0;
		*x0 = 0;
	}

	if (*y0 < 0) {
		*h += *y0;
		if (ymod) *ymod -= *y0;
		*y0 = 0;
	}

	if (*x0 + *w >= SCREEN_WIDTH) *w -= *x0 + *w - SCREEN_WIDTH;
	if (*y0 + *h >= SCREEN_HEIGHT) *h -= *y0 + *h - SCREEN_HEIGHT;

	if (*w <= 0 || *h <= 0) return 0;

	return 1;
}

static void screen_draw_rect(uint32_t* screen, int x0, int y0, int w, int h, uint32_t color)
{
	if (!screen_clip_rect(&x0, &y0, &w, &h, NULL, NULL)) return;
	
	int i = x0 + y0 * SCREEN_WIDTH;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			screen[i++] = color;
		}
		i += SCREEN_WIDTH - w;
	}
}

struct img {
	uint32_t* data;
	int width;
	int height;
	int bpp;
};


static void img_load(struct img* img, const char* asset)
{
	img->data = (uint32_t*)stbi_load(asset_path(asset), &img->width, &img->height, &img->bpp, 4);
	AN(img->data);
}


static void screen_draw_img(uint32_t* screen, struct img* img, int x0, int y0, int x1, int y1, int w, int h)
{
	if (!screen_clip_rect(&x1, &y1, &w, &h, &x0, &y0)) return;

	int i0 = x0 + y0 * img->width;
	int i1 = x1 + y1 * SCREEN_WIDTH;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t s0 = img->data[i0];
			if ((s0 & 0xffffff) != 0xff00ff) screen[i1] = s0;
			i0++;
			i1++;
		}
		i0 += img->width - w;
		i1 += SCREEN_WIDTH - w;
	}
}

static void screen_draw_img_color(uint32_t* screen, struct img* img, int x0, int y0, int x1, int y1, int w, int h, uint32_t color)
{
	if (!screen_clip_rect(&x1, &y1, &w, &h, &x0, &y0)) return;

	int i0 = x0 + y0 * img->width;
	int i1 = x1 + y1 * SCREEN_WIDTH;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t s0 = img->data[i0];
			if ((s0 & 0xffffff) != 0xff00ff) screen[i1] = color;
			i0++;
			i1++;
		}
		i0 += img->width - w;
		i1 += SCREEN_WIDTH - w;
	}
}

static void screen_draw_img_pain(uint32_t* screen, struct img* img, int x0, int y0, int x1, int y1, int w, int h, int pain)
{
	if (pain) {
		screen_draw_img_color(screen, img, x0, y0, x1, y1, w, h, mkcol(255,255,255));
	} else {
		screen_draw_img(screen, img, x0, y0, x1, y1, w, h);
	}
}

struct font {
	struct img img;
	int x0;
	int x;
	int y;
	char* buffer;
	uint32_t color;
};

static void font_init(struct font* font)
{
	memset(font, 0, sizeof(*font));
	img_load(&font->img, "font6.png");
	font->buffer = malloc(4096);
	AN(font->buffer);
}

static void font_set_color(struct font* font, uint32_t color)
{
	font->color = color;
}

static void font_set_cursor(struct font* font, int x, int y)
{
	font->x0 = x;
	font->x = x;
	font->y = y;
}

void font_printf(struct font* font, uint32_t* screen, const char* fmt, ...) __attribute__((format (printf, 3, 4)));
void font_printf(struct font* font, uint32_t* screen, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = vsprintf(font->buffer, fmt, args);
	for (int i = 0; i < n; i++) {
		unsigned char ch = font->buffer[i];
		if (ch == '\n') {
			font->x = font->x0;
			font->y += 9;
			continue;
		}
		int x0 = (ch & 15) * 6;
		int y0 = (ch >> 4) * 6;
		screen_draw_img_color(screen, &font->img, x0, y0, font->x, font->y, 6, 6, font->color);
		font->x += 6;
	}
	va_end(args);
}


static uint32_t drum_color(int drum_id)
{
	switch (drum_id) {
		case DRUM_ID_KICK: return mkcol(225,32,6);
		case DRUM_ID_SNARE: return mkcol(254,200,69);
		case DRUM_ID_HIHAT: return mkcol(110,206,237);
		case DRUM_ID_OPEN: return mkcol(255,255,255);
		default: return 0;
	}
}

static uint32_t drum_color_light(int drum_id)
{
	switch (drum_id) {
		case DRUM_ID_KICK: return mkcol(251,130,114);
		case DRUM_ID_SNARE: return mkcol(254,226,159);
		case DRUM_ID_HIHAT: return mkcol(179,229,246);
		case DRUM_ID_OPEN: return mkcol(255,255,255);
		default: return 0;
	}
}

static uint32_t drum_color_overlay(int drum_id)
{
	switch (drum_id) {
		case DRUM_ID_KICK: return mkcol(101,14,2);
		case DRUM_ID_SNARE: return mkcol(165,117,0);
		case DRUM_ID_HIHAT: return mkcol(19,127,162);
		case DRUM_ID_OPEN: return mkcol(146,146,146);
		default: return 0;
	}
}

static uint32_t drum_color_dim(int  drum_id)
{
	switch (drum_id) {
		case DRUM_ID_KICK: return mkcol(47,4,0);
		case DRUM_ID_SNARE: return mkcol(82,54,0);
		case DRUM_ID_HIHAT: return mkcol(9,63,78);
		case DRUM_ID_OPEN: return mkcol(74,74,74);
		default: return 0;
	}
}


static void piano_roll_render(struct piano_roll* piano_roll, uint32_t* screen, struct font* font)
{
	float width_in_seconds = 5.0f;
	float offset_in_seconds = 1.05f;

	float bps = piano_roll->song->bpm / 60.0;

	float width_in_beats = width_in_seconds * bps;
	float offset_in_beats = offset_in_seconds * bps;

	float current_beat = piano_roll->time_in_seconds * bps;

	float x0 = (SCREEN_WIDTH * offset_in_seconds) / width_in_seconds;

	// render bars
	int b0 = (int)floorf(current_beat - offset_in_beats);
	int b1 = b0 + (int)ceilf(width_in_beats) + 1;
	float beat_width = SCREEN_WIDTH / width_in_beats;
	for (int b = b0; b <= b1; b++) {
		for (int sub = 0; sub < 4; sub++) {
			if (sub&1) continue;
			float bf = (float)b + (float)sub * 0.25f;

			float x = x0 + (bf - current_beat) * beat_width;

			int width = 0;
			int th = 0;
			if (sub == 0) {
				if ((b % piano_roll->song->time_signature) == 0) {
					width = 3;
					th = 7;
				} else {
					width = 1;
					th = 3;
				}
			} else {
				width = 1;
				th = 1;
			}


			for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
				uint32_t color = 0;
				if (sub == 0) {
					color = drum_color_overlay(drum_id);
				} else {
					color = drum_color_dim(drum_id);
				}
				screen_draw_rect(screen, (int)x-width/2-1, 17 + drum_id*9, width, 7, color);
			}

			screen_draw_rect(screen, (int)x-width/2-1, 8 + 7-th, width, th, mkcol(255,255,255));
			screen_draw_rect(screen, (int)x-width/2-1, 53, width, th, mkcol(255,255,255));
		}
	}

	int spacing = 9;
	int y0 = 16;

	// render song
	int s0 = (int)floorf((current_beat - offset_in_beats) * piano_roll->song->lpb);
	int s1 = s0 + (int)ceil(width_in_beats * piano_roll->song->lpb)+1;
	float step_width = (SCREEN_WIDTH / width_in_beats) / piano_roll->song->lpb;
	float current_step = current_beat * piano_roll->song->lpb;
	for (int s = s0; s <= s1; s++) {
		if (s < 0 || s >= piano_roll->song->length) continue;
		float x = x0 + ((float)s - current_step) * step_width;
		int dctl = piano_roll->song->drums[s];

		for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
			int mask = 1<<drum_id;
			if (!(dctl & mask)) continue;
			// int width =5;
			//screen_draw_rect(screen, (int)x-width/2, y0+drum_id*spacing, 3, 2, drum_color_light(drum_id));
			//screen_draw_rect(screen, (int)x-width/2, y0+drum_id*spacing+7, 3, 2, drum_color_light(drum_id));
			screen_draw_rect(screen, (int)x-2-1, y0+drum_id*spacing+2-1, 7, 7, 0);
			screen_draw_rect(screen, (int)x-2, y0+drum_id*spacing+2, 5, 5, drum_color_light(drum_id));
		}
	}

	// render played notes
	float second_width = (float)SCREEN_WIDTH / width_in_seconds;
	for (int i = 0; i < MAX_PLAYED_NOTES; i++) {
		struct played_note* note = &piano_roll->played_notes[i];
		if (note->time_in_seconds <= 0.0) continue;
		float dt = note->time_in_seconds - piano_roll->time_in_seconds;
		float x = x0 + dt * second_width;
		//screen_draw_rect(screen, (int)x-2, y0+note->drum_id*spacing+2, 5, 5, drum_color(note->drum_id));
		screen_draw_rect(screen, (int)x-5/2, y0+note->drum_id*spacing, 3, 2, drum_color(note->drum_id));
		screen_draw_rect(screen, (int)x-5/2, y0+note->drum_id*spacing+7, 3, 2, drum_color(note->drum_id));
	}

	// render gauge
	{
		int dx = piano_roll->gauge * SCREEN_WIDTH;
		if (dx < 0) dx = 0;
		if (dx >= SCREEN_WIDTH) dx = SCREEN_WIDTH;
		int green = piano_roll->gauge * 255;
		if (green < 0) green = 0;
		if (green > 255) green = 255;
		int red = 255 - piano_roll->gauge * 255;
		if (red < 0) red = 0;
		if (red > 255) red = 255;
		screen_draw_rect(screen, 0, 201, dx, 7, mkcol(red,green,0));

		font_set_color(font, 0);
		font_set_cursor(font, 1, 202);
		font_printf(font, screen, "awesomemeter");
	}
}

static void present_screen(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, uint32_t* screen)
{
	//SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	//SDL_RenderClear(renderer);

	SDL_UpdateTexture(texture, NULL, screen, SCREEN_WIDTH * sizeof(uint32_t));

	SDL_DisplayMode mode;
	SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &mode);

	int window_width = mode.w;
	int window_height = mode.h;

	float window_aspect = (float)window_width / (float)window_height;
	float screen_aspect = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;

	SDL_Rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = SCREEN_WIDTH;
	rect.h = SCREEN_HEIGHT;

	if (screen_aspect > window_aspect) {
		rect.w = window_width;
		rect.h = (window_width * SCREEN_HEIGHT) / SCREEN_WIDTH;
		rect.x = 0;
		rect.y = (window_height - rect.h) / 2;
	} else {
		rect.w = (window_height * SCREEN_WIDTH) / SCREEN_HEIGHT;
		rect.h = window_height;
		rect.x = (window_width - rect.w) / 2;
		rect.y = 0;
	}

	ASSERT(rect.w > 0);
	ASSERT(rect.h > 0);
	ASSERT(rect.x >= 0);
	ASSERT(rect.y >= 0);

	SDL_RenderCopy(renderer, texture, NULL, &rect);

	SDL_RenderPresent(renderer);
}


#define MAX_GIBLETS (512)

struct giblet {
	int type;
	int owner;
	float x;
	float y;
	float vx;
	float vy;
	int active;
	int grounded;
};


struct giblet_exploder {
	struct img img;

	// state
	int next_giblet;
	struct giblet* giblets;
	struct rng rng;
};

static void giblet_exploder_init(struct giblet_exploder* gx)
{
	memset(gx, 0, sizeof(*gx));
	gx->giblets = calloc(MAX_GIBLETS, sizeof(*gx->giblets));
	// height:7
	// number: 8
	img_load(&gx->img, "gilbets.png");
}

static void giblet_exploder_reset(struct giblet_exploder* gx)
{
	gx->next_giblet = 0;
	memset(gx->giblets, 0, sizeof(*gx->giblets) * MAX_GIBLETS);
	rng_seed(&gx->rng, 666);
}

static void giblet_exploder_bang(struct giblet_exploder* gx, int x0, int y0, int w, int h, int owner)
{
	int count = 50;
	float max_speed = 40;
	for (int i = 0; i < count; i++) {
		struct giblet* g = &gx->giblets[gx->next_giblet++];
		if (gx->next_giblet >= MAX_GIBLETS) gx->next_giblet = 0;

		g->active = 1;
		g->grounded = 0;
		g->owner = owner;
		g->x = (float)x0 + (float)w * rng_float(&gx->rng);
		g->y = (float)y0 + (float)h * rng_float(&gx->rng);
		g->vx = (rng_float(&gx->rng) * 2.0 - 1.0) * max_speed;
		g->vy = (rng_float(&gx->rng) * 2.0 - 1.0) * max_speed;
		g->type = rng_uint32(&gx->rng) & 7;
	}
}

static void giblet_exploder_update(struct giblet_exploder* gx, float dt)
{
	float gravity = 40.0f;
	int base_floor_y = 169;

	for (int i = 0; i < MAX_GIBLETS; i++) {
		struct giblet* g = &gx->giblets[i];
		if (!g->active || g->grounded) continue;
		g->vy += gravity * dt;
		g->x += g->vx * dt;
		g->y += g->vy * dt;
		struct rng rng;
		rng_seed(&rng, g->owner * 54531 + i);
		int floor_dy = rng_uint32(&rng) % 28;
		int floor_y = base_floor_y + floor_dy - 14;
		if (g->y > floor_y) {
			g->y = floor_y;
			g->grounded = 1;
			g->owner = 0;
		}
	}
}

static void giblet_exploder_render(struct giblet_exploder* gx, uint32_t* screen, int owner)
{
	for (int i = 0; i < MAX_GIBLETS; i++) {
		struct giblet* g = &gx->giblets[i];
		if (!g->active) continue;
		if (g->owner != owner) continue;
		int x = (int)g->x;
		int y = (int)g->y;
		screen_draw_img(screen, &gx->img, g->type * 12, 0, x, y, 12, 7);
	}
}


#define MAX_ZOMBIES (128)

struct zombie {
	int active;
	int frame;
	int style;
	int pause;
	int stagger;
	int gib;
	int x;
	int y;
	int giblet_owner;
};

static int zombie_effective_x(struct zombie* z)
{
	const int dx [] = {
		120, // frame 1
		115, // frame 2
		104, // frame 3
		91,  // frame 4
		102, // frame 5
		99,  // frame 6
		90,  // frame 7
		85,  // frame 8
		75,  // frame 9
		61,  // frame 10
		72,  // frame 11
		69,  // frame 12

	};
	return z->x + dx[z->frame];
}

struct zombie_director {
	struct img imgs[10];
	struct rng rng;

	// state
	float dt_accum;
	struct zombie* zombies;
	int spawn_counter;
	int ticks;
	int next_giblet_owner;
};


static void zombie_director_init(struct zombie_director* zd)
{
	memset(zd, 0, sizeof*zd);

	zd->zombies = calloc(MAX_ZOMBIES, sizeof(*zd->zombies));
	AN(zd->zombies);

	const char* zs[] = {
		"zombiep0.png",
		"zombiep1.png",
		"zombiep2.png",
		"zombiep3.png",
		"zombiep4.png",
		"zombiep5.png",
		"zombiep6.png",
		"zombiep7.png",
		"zombiep8.png",
		"zombiep9.png"
	};

	for (int i = 0; i < 10; i++) {
		// zombiep.png PNG 163x984 163x984+0+0 8-bit sRGB 12c 5.74KB 0.000u 0:00.000
		struct img* img = &zd->imgs[i];
		img_load(img, zs[i]);
		ASSERT(img->width == 163);
		ASSERT(img->height == 984);
	}
}

static void zombie_director_reset(struct zombie_director* zd)
{
	rng_seed(&zd->rng, 420);
	zd->dt_accum = 0;
	memset(zd->zombies, 0, sizeof(*zd->zombies) * MAX_ZOMBIES);
	zd->spawn_counter = 0;
	zd->ticks = 0;
	zd->next_giblet_owner = 0;
}

static void zombie_director_update(struct zombie_director* zd, struct piano_roll* piano_roll, float dt, struct giblet_exploder* gx)
{
	float tick_time = 0.05f;
	int spawn_ticks = 12;
	int zombie_start_x = 270;
	int zombie_start_y = 75;
	int zombie_start_y_spread = 30;
	int zombie_cycle_dx = 60;
	int zombie_frame_count = 12;
	int gib_duration = 7;

	zd->dt_accum += dt;

	float gauge = piano_roll->gauge;

	while (zd->dt_accum > 0) {
		// handle zombie spawning
		if (zd->spawn_counter > spawn_ticks) {
			float rf = rng_float(&zd->rng);
			if (rf > (gauge * 0.8)) {
				struct zombie* found = NULL;
				for (int i = 0; i < MAX_ZOMBIES; i++) {
					struct zombie* z = &zd->zombies[i];
					if (!z->active) {
						found = z;
						break;
					}
				}
				if (found != NULL) {
					uint32_t ri = rng_uint32(&zd->rng);
					found->active = 1;
					found->frame = 0;
					found->x = zombie_start_x;
					found->y = zombie_start_y + (ri%zombie_start_y_spread);
					found->style = (ri>>5)%10;
					found->giblet_owner = ++zd->next_giblet_owner;
				}
			}

			zd->spawn_counter = 0;
		}

		// animate the dead
		for (int i = 0; i < MAX_ZOMBIES; i++) {
			struct zombie* z = &zd->zombies[i];

			if (!z->active) continue;

			// zombie gib animation
			if (z->gib) {
				z->gib++;
				if (z->gib > gib_duration) {
					z->active = 0;
					giblet_exploder_bang(
						gx, 
						zombie_effective_x(z) + 24,
						z->y + 11,
						18,
						71,
						z->giblet_owner);
					continue;
				}
			}

			if (z->gib) continue;

			if (zd->ticks&1) continue;

			// handle zombie death
			int effective_x = zombie_effective_x(z);
			float effective_xf = (float)effective_x / (float)SCREEN_WIDTH;
			int zombie_must_die = 0;
			if (effective_xf < gauge*0.8) {
				zombie_must_die = 1;
			}
			if (zombie_must_die) z->gib = 1;
			
			// pause
			if (z->pause > 0) {
				z->pause--;
				continue;
			}
			
			// staggering and anim personality
			uint32_t ri = rng_uint32(&zd->rng);
			if (z->stagger) {
				z->stagger = 0;
				z->frame--;
			} else {
				z->frame++;
			}
			switch (z->frame) {
				case 0: case 6: // stagger?
					z->stagger = ((ri/10)%3) == 0;
					z->pause = ri%4;
					break;
				case 11: case 5: // stagger?
					z->pause = ri%4;
					break;
				case 3: case 9: // pause?
					z->pause = ri%4;
					break;
			}

			// wrap / advance x
			if (z->frame >= zombie_frame_count) {
				z->x -= zombie_cycle_dx;
				z->frame -= zombie_frame_count;
			} else if (z->frame < 0) {
				z->x += zombie_cycle_dx;
				z->frame += zombie_frame_count;
			}
		}

		zd->spawn_counter++;
		zd->ticks++;
		zd->dt_accum -= tick_time;
	}
}

static int zombie_director_get_leftmost_x(struct zombie_director* zd)
{
	int x = SCREEN_WIDTH;
	for (int i = 0; i < MAX_ZOMBIES; i++) {
		struct zombie* z = &zd->zombies[i];
		if (!z->active) continue;
		int zx = zombie_effective_x(z);
		if (zx < x) x = zx;
	}
	return x;

}

static int zombie_y_sort(const void* va, const void* vb)
{
	const struct zombie* a = va;
	const struct zombie* b = vb;
	return a->y - b->y;
}

static void zombie_director_render(struct zombie_director* zd, uint32_t* screen, struct giblet_exploder* gx)
{
	qsort(zd->zombies, MAX_ZOMBIES, sizeof(struct zombie), zombie_y_sort);
	int anim_offset = 82;
	for (int i = 0; i < MAX_ZOMBIES; i++) {
		struct zombie* z = &zd->zombies[i];
		giblet_exploder_render(gx, screen, z->giblet_owner);
		if (!z->active) continue;
		int pain = z->gib & 1;
		screen_draw_img_pain(screen, &zd->imgs[z->style], 0, anim_offset * z->frame, z->x, z->y, 163, anim_offset, pain);
	}
}


struct drummer {
	struct img img;
	int drum_control;
	int x;
	int y;
	int kill_dx;

	float dt_accum;
	int gib;
	int dead;

	int giblet_owner;
};

static void drummer_init(struct drummer* drummer)
{
	// drummerp.png PNG 150x240 150x240+0+0 8-bit sRGB 26c 1.77KB 0.000u 0:00.000
	img_load(&drummer->img, "drummerp.png");
	ASSERT(drummer->img.width == 150);
	ASSERT(drummer->img.height == 240);

}

static void drummer_reset(struct drummer* drummer)
{
	struct img save = drummer->img;
	memset(drummer, 0, sizeof(*drummer));
	drummer->img = save;
	drummer->x = 45;
	drummer->y = 112;
	drummer->kill_dx = 14;
	drummer->giblet_owner = -1000;
}

static void drummer_update(struct drummer* drummer, int drum_control, struct zombie_director* zombie_director, float dt, struct giblet_exploder* gx)
{
	drummer->drum_control = drum_control;
	if (drummer->dead) return;

	drummer->dt_accum += dt;

	float tick_time = 0.05f;
	int gib_duration = 10;
	int drummer_effective_x = drummer->x + drummer->kill_dx;

	while (drummer->dt_accum > 0) {
		if (drummer->gib) {
			if (drummer->gib > gib_duration) {
				drummer->dead = 1;
				giblet_exploder_bang(
					gx,
					drummer->x,
					drummer->y,
					50,
					90,
					drummer->giblet_owner);
				return;
			}
			drummer->gib++;
		}
		if (!drummer->gib && zombie_director_get_leftmost_x(zombie_director) < drummer_effective_x) {
			drummer->gib++;
		}
		drummer->dt_accum -= tick_time;
	}
}

static void drummer_render(struct drummer* drummer, uint32_t* screen, struct giblet_exploder* gx)
{
	giblet_exploder_render(gx, screen, drummer->giblet_owner);
	#if 0
	// src offset, src dim - draw offset
	histick 13,96 21x7 - 14,28
	hihat 4,110 20x7 - 24,33
	head 2,29 17x21 - 1,0
	kick 1,1 30x26 - 0,39
	snare 3,61 34x23 - 3,18
	static 40,1 26x26 - 27,39
	#endif

	if (drummer->dead) return;

	struct img* img = &drummer->img;
	int drum_control = drummer->drum_control;

	int x = drummer->x;
	int y = drummer->y;
	int actoff = img->height >> 1;
	int pain = drummer->gib&1;

	// histick
	screen_draw_img_pain(screen, img, 13, 96 + ((drum_control & DRUM_CONTROL_HIHAT) ? actoff : 0), x+14, y+28, 21, 7, pain);

	// hihat
	screen_draw_img_pain(screen, img, 4, 110 + ((drum_control & DRUM_CONTROL_OPEN) ? actoff : 0), x+24, y+33, 20, 7, pain);

	// head
	screen_draw_img_pain(screen, img, 2, 29 + ((drum_control & DRUM_CONTROL_HEAD) ? actoff : 0), x+1, y+0, 17, 21, pain);

	// kick
	screen_draw_img_pain(screen, img, 1, 1 + ((drum_control & DRUM_CONTROL_KICK) ? actoff : 0), x+0, y+39, 30, 26, pain);

	// snare
	screen_draw_img_pain(screen, img, 3, 61 + ((drum_control & DRUM_CONTROL_SNARE) ? actoff : 0), x+3, y+18, 34, 23, pain);

	// static
	screen_draw_img_pain(screen, img, 40, 1, x+27, y+39, 26, 26, pain);

}


struct player {
	// setup
	struct img img;
	int width, height;
	int x, y;
	int anim_offset;
	int frames;
	int kill_dx;
	int giblet_owner;

	// state
	int gib;
	int dead;
	float dt_accum;
};

static void player_init(struct player* player, const char* asset, int width, int height, int x, int y, int anim_offset, int frames, int kill_dx, int giblet_owner)
{
	memset(player, 0, sizeof(*player));
	img_load(&player->img, asset);
	ASSERT(player->img.width == width);
	ASSERT(player->img.height == height);
	player->width = width;
	player->height = height;
	player->x = x;
	player->y = y;
	player->anim_offset = anim_offset;
	player->frames = frames;
	player->kill_dx = kill_dx;
	player->giblet_owner = giblet_owner;
}

static void player_reset(struct player* player)
{
	player->gib = 0;
	player->dead = 0;
	player->dt_accum = 0;
}

static int player_effective_x(struct player* player)
{
	return player->x + player->kill_dx;
}

static void player_update(struct player* p, struct zombie_director* zombie_director, float dt, struct giblet_exploder* gx)
{
	if (p->dead) return;

	p->dt_accum += dt;

	float tick_time = 0.05f;
	int gib_duration = 7;

	while (p->dt_accum > 0) {
		if (p->gib) {
			if (p->gib > gib_duration) {
				p->dead = 1;
				giblet_exploder_bang(
					gx,
					p->x,
					p->y,
					40,
					100,
					p->giblet_owner);
				return;
			}
			p->gib++;
		}
		if (!p->gib && zombie_director_get_leftmost_x(zombie_director) < player_effective_x(p)) {
			p->gib++;
		}
		p->dt_accum -= tick_time;
	}
}

static void player_render(struct player* player, uint32_t* screen, int step, struct giblet_exploder* gx)
{
	giblet_exploder_render(gx, screen, player->giblet_owner);

	if (player->dead) return;

	int frame = (step>>1) % player->frames;
	int pain = player->gib & 1;
	screen_draw_img_pain(
		screen,
		&player->img,
		0,
		frame * player->anim_offset,
		player->x,
		player->y,
		player->width,
		player->anim_offset,
		pain);
}

int main(int argc, char** argv)
{
	SAZ(SDL_Init(SDL_INIT_EVERYTHING));
	atexit(SDL_Quit);

	{
		char* sdl_base_path = SDL_GetBasePath();
		if (sdl_base_path) {
			if (strlen(sdl_base_path) > (sizeof(assets_base)-1)) arghf("SDL_GetBasePath() too long");
			strcpy(assets_base, sdl_base_path);
			SDL_free(sdl_base_path);
		} else {
			arghf("SDL_GetBasePath() = NULL");
		}
	}

	#if 1
	SDL_Window* window = SDL_CreateWindow(
			"Drums of the Dead",
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			0, 0,
			SDL_WINDOW_FULLSCREEN_DESKTOP);
	#else
	SDL_Window* window = SDL_CreateWindow(
			"Drums of the Dead",
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			500, 1000,
			0);
	#endif
	SAN(window);

	SDL_DisplayMode mode;
	SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(window), &mode);

	int refresh_rate = mode.refresh_rate;
	if (!refresh_rate) refresh_rate = 60; // #pray

	SDL_Renderer* renderer = SDL_CreateRenderer(
			window,
			-1, 
			SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SAN(renderer);

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);


	SDL_Texture* texture = SDL_CreateTexture(
			renderer,
			SDL_PIXELFORMAT_ABGR8888,
			SDL_TEXTUREACCESS_STREAMING,
			SCREEN_WIDTH,
			SCREEN_HEIGHT);
	SAN(texture);

	struct audio audio;
	audio_init(&audio);

	struct piano_roll piano_roll;
	piano_roll_init(&piano_roll, &song_data_song);

	uint32_t feedback_cursor = 0;

	uint8_t drum_control_keymap[128];
	memset(drum_control_keymap, 0, 128);

	// XXX hard-coded for now, TODO config, TODO prefs (SDL_GetPrefPath())
	drum_control_keymap['z'] = DRUM_CONTROL_KICK;
	drum_control_keymap['x'] = DRUM_CONTROL_KICK;
	drum_control_keymap['c'] = DRUM_CONTROL_SNARE;
	drum_control_keymap['v'] = DRUM_CONTROL_SNARE;
	drum_control_keymap['n'] = DRUM_CONTROL_HIHAT;
	drum_control_keymap['m'] = DRUM_CONTROL_HIHAT;
	drum_control_keymap[','] = DRUM_CONTROL_OPEN;

	struct font font;
	font_init(&font);

	struct img bg_img;
	// assets/background.png PNG 384x216 384x216+0+0 8-bit sRGB 256c 2.22KB 0.000u 0:00.000
	img_load(&bg_img, "background.png");
	ASSERT(bg_img.width == SCREEN_WIDTH);
	ASSERT(bg_img.height == SCREEN_HEIGHT);

	struct giblet_exploder giblet_exploder;
	giblet_exploder_init(&giblet_exploder);

	struct drummer drummer;
	drummer_init(&drummer);

	struct player bass_player;
	player_init(
		&bass_player,
		"bassp.png",
		88, 400,
		78, 101,
		80,
		5,
		42,
		-1);

	struct player guitar_player;
	player_init(
		&guitar_player,
		"guitarp.png",
		65, 534,
		120, 96,
		89,
		6,
		34,
		-2);

	struct zombie_director zombie_director;
	zombie_director_init(&zombie_director);

	int drum_control_cooldown[DRUM_ID_MAX] = {0};

	struct img menu_img;
	img_load(&menu_img, "menu.png");
	ASSERT(menu_img.width == SCREEN_WIDTH);
	ASSERT(menu_img.height == SCREEN_HEIGHT);

	uint32_t* screen = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
	AN(screen);

	int exiting = 0;
	int menu = 1;
	int menu_selection = 0;
	int audio_buffer_length_exp = 1;
	while (!exiting) {
		//uint64_t t0 = SDL_GetPerformanceCounter();
		
		if (menu) {
			SDL_Event e;
			int select = 0;
			int pressed_char = 0;
			int menu_length = 7;
			int d = 0;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT) exiting = 1;
				if (e.type == SDL_KEYDOWN) {
					if (e.key.keysym.sym == SDLK_ESCAPE) {
						exiting = 1;
					} else if (e.key.keysym.sym == SDLK_RETURN) {
						select = 1;
					} else if (e.key.keysym.sym == SDLK_UP) {
						menu_selection--;
						if (menu_selection < 0)  menu_selection += menu_length;
					} else if (e.key.keysym.sym == SDLK_DOWN) {
						menu_selection++;
						if (menu_selection >= menu_length)  menu_selection -= menu_length;
					} else if (e.key.keysym.sym == SDLK_LEFT) {
						d = -1;
					} else if (e.key.keysym.sym == SDLK_RIGHT) {
						d = 1;
					} else {
						int k = e.key.keysym.sym;
						if (k >= 32 && k < 128) {
							pressed_char = k;
						}
					}
				}
			}

			switch (menu_selection) {
				case 0:
					if (select) {
						menu = 0;
						drummer_reset(&drummer);
						player_reset(&bass_player);
						player_reset(&guitar_player);
						zombie_director_reset(&zombie_director);
						piano_roll_reset(&piano_roll);
						giblet_exploder_reset(&giblet_exploder);
						audio_start(&audio, audio_buffer_length_exp);
					}
					break;
				case 5:
					audio_buffer_length_exp = (audio_buffer_length_exp + d) % 4;
					if (audio_buffer_length_exp < 0) audio_buffer_length_exp = 3;
					break;
				case 6:
					if (select) {
						exiting = 1;
					}
					break;
				case 1: case 2: case 3: case 4:
					if (pressed_char) {
						drum_control_keymap[pressed_char] = (1<<(menu_selection-1));
					}
					break;
			}


			memcpy(screen, menu_img.data, sizeof(uint32_t) * SCREEN_WIDTH * SCREEN_HEIGHT);

			uint32_t select_color = mkcol(255,255,255);
			uint32_t unselect_color = mkcol(128,128,128);

			font_set_color(&font, menu_selection == 0 ? select_color : unselect_color);
			font_set_cursor(&font, 120, 100);
			font_printf(&font, screen, "start game\n");

			const char* drum_names[] = {
				"kick",
				"snare",
				"hihat",
				"open"
			};

			for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
				font_set_color(&font, menu_selection == (1+drum_id) ? select_color : unselect_color);
				font_printf(&font, screen, "%s keys: ", drum_names[drum_id]);
				for (int c = 32; c < 128; c++) {
					if (drum_control_keymap[c] & (1<<drum_id)) {
						font_printf(&font, screen, "%c", c);
					}
				}
				font_printf(&font, screen, "\n");
			}

			font_set_color(&font, menu_selection == 5 ? select_color : unselect_color);
			font_printf(&font, screen, "audio buffer length: %d\n", 256 << audio_buffer_length_exp);
			font_set_color(&font, menu_selection == 6 ? select_color : unselect_color);
			font_printf(&font, screen, "quit to dos");

			font_set_color(&font, mkcol(255,200,150));
			font_set_cursor(&font, 30, 70);
			font_printf(&font, screen, "your last gig sucked so much that you raised the dead\n");
			font_set_color(&font, mkcol(255,255,255));
			font_set_cursor(&font, 1, 80);
			font_printf(&font, screen, "now play some awesome drums or else the zombies will devour you!");
		} else {
			SDL_Event e;
			uint32_t drum_control = 0;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT) exiting = 1;
				if (e.type == SDL_KEYDOWN) {
					if (e.key.keysym.sym == SDLK_ESCAPE) {
						menu = 1;
						audio_stop(&audio);
					}

					int k = e.key.keysym.sym;
					if (k >= 32 && k < 128) {
						drum_control |= drum_control_keymap[k];
					}
				}
			}

			uint32_t audio_position;

			if (drummer.dead) {
				drum_control = 0;
			}

			audio_lock(&audio);
			{
				audio_position = audio.position;
				while (feedback_cursor != audio.drum_control_feedback_cursor) {
					struct drum_control_feedback* fb = &audio.drum_control_feedback[feedback_cursor];
					if (fb->value) {
						piano_roll_register_drum_control_feedback(&piano_roll, &audio, fb);
					}
					feedback_cursor = (feedback_cursor + 1) & DRUM_CONTROL_FEEDBACK_MASK;
				}
				audio_emit_drum_control(&audio, drum_control);

				// handle player death
				if (bass_player.dead) audio.bass_stopped = 1;
				if (guitar_player.dead) audio.guitar_stopped = 1;
			}
			audio_unlock(&audio);


			memset(screen, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));


			uint32_t cool_drum_control = 0;
			for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
				int mask = 1<<drum_id;
				if (drum_control & mask) {
					int cooldown = drum_id == DRUM_ID_OPEN ? refresh_rate * 2 : refresh_rate / 10;
					drum_control_cooldown[drum_id] = cooldown;
					// open/close hihack
					if (drum_id == DRUM_ID_HIHAT) drum_control_cooldown[DRUM_ID_OPEN] = 0;
				}
				if (drum_control_cooldown[drum_id] > 0) {
					cool_drum_control |= mask;
					drum_control_cooldown[drum_id]--;
				}
			}

			int step = (int)((audio_position_to_seconds(&audio, audio_position) * (float)piano_roll.song->bpm * (float)piano_roll.song->lpb) / 60.0);

			int song_end = step > piano_roll.song->length;

			if (step & 4 && !song_end) {
				cool_drum_control |= DRUM_CONTROL_HEAD;
			}

			piano_roll_update_position(&piano_roll, &audio, audio_position);
			piano_roll_update_gauge(&piano_roll);

			float dt = 1.0f / (float)refresh_rate;

			zombie_director_update(&zombie_director, &piano_roll, dt, &giblet_exploder);
			drummer_update(&drummer, cool_drum_control, &zombie_director, dt, &giblet_exploder);
			player_update(&bass_player, &zombie_director, dt, &giblet_exploder);
			player_update(&guitar_player, &zombie_director, dt, &giblet_exploder);
			giblet_exploder_update(&giblet_exploder, dt);

			memcpy(screen, bg_img.data, SCREEN_WIDTH*SCREEN_HEIGHT*sizeof(uint32_t));

			giblet_exploder_render(&giblet_exploder, screen, 0);
			drummer_render(&drummer, screen, &giblet_exploder);
			player_render(&bass_player, screen, song_end ? 0 : step, &giblet_exploder);
			player_render(&guitar_player, screen, song_end ? 0 : step, &giblet_exploder);
			if (drummer.dead) {
				screen_draw_rect(screen, 0, 0, SCREEN_WIDTH, 64, 0);
			} else {
				piano_roll_render(&piano_roll, screen, &font);
			}

			zombie_director_render(&zombie_director, screen, &giblet_exploder);


			//uint64_t frame_time = SDL_GetPerformanceCounter() - t0;
			//printf("%lu\n", frame_time);
		}
		present_screen(window, renderer, texture, screen);
	}

	audio_quit(&audio);

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	return EXIT_SUCCESS;
}
