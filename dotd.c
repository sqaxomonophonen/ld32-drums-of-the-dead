#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "platform.h"

#include "a.h"

// SONGS
#include "song.h"
#include "test.xrns.inc.c"

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

struct audio {
	SDL_AudioDeviceID device;
	uint32_t sample_rate;
	struct rng rng;

	SDL_mutex* mutex;
	uint32_t drum_control;
	int drum_control_feedback_cursor;
	struct drum_control_feedback drum_control_feedback[DRUM_CONTROL_FEEDBACK_N];

	uint32_t position;

	struct drum_samples drum_samples;

	struct sample_ctx drum_sample_ctx[DRUM_ID_MAX];
};

static void audio_lock(struct audio* audio)
{
	SAZ(SDL_LockMutex(audio->mutex));
}

static void audio_unlock(struct audio* audio)
{
	SDL_UnlockMutex(audio->mutex);
}

static void audio_callback(void* userdata, Uint8* stream_u8, int bytes)
{
	struct audio* audio = userdata;
	float* stream = (float*)stream_u8;
	int n = bytes / sizeof(float) / 2;

	audio_lock(audio);
	uint32_t drum_control = audio->drum_control;
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

	// generate samples
	int p = 0;
	for (int i = 0; i < n; i++) {
		float out[2] = {0,0};

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
		f->value = audio->drum_control;

		// clear drum control
		audio->drum_control = 0;

	}
	audio_unlock(audio);

	audio->position += n;

}

static void audio_init(struct audio* audio)
{
	memset(audio, 0, sizeof(*audio));

	rng_seed(&audio->rng, 0);

	audio->mutex = SDL_CreateMutex();
	SAN(audio->mutex);

	drum_samples_init(&audio->drum_samples);

	SDL_AudioSpec want, have;
	want.freq = 44100;
	want.format = AUDIO_F32;
	want.channels = 2;
	want.samples = 512;
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

	SDL_PauseAudioDevice(audio->device, 0);
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
	float time_in_seconds;
	struct played_note* played_notes;
	struct song* song;
	float gauge;
	int gauge_last_step;
};

static void piano_roll_init(struct piano_roll* p, struct song* song)
{
	memset(p, 0, sizeof(*p));
	p->song = song;
	p->gauge = 1.0f;
	p->played_notes = calloc(MAX_PLAYED_NOTES, sizeof(*p->played_notes));
	AN(p->played_notes);
}

static void piano_roll_update_position(struct piano_roll* p, struct audio* audio, uint32_t audio_position)
{
	p->time_in_seconds = audio_position_to_seconds(audio, audio_position);
}

static void piano_roll_gauge_dstep(struct piano_roll* p, int match, float dstep)
{
	float dpenalty = match ? (dstep / (float)p->song->lpb) : 1.0f;
	if (dpenalty > 1.0f) dpenalty = 1.0f;
	float good_threshold = 0.25f;
	p->gauge -= ((dpenalty * dpenalty) - (good_threshold * good_threshold)) * 0.1f;
}

static void piano_roll_update_gauge(struct piano_roll* p)
{
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


static uint32_t drum_color(drum_id)
{
	switch (drum_id) {
		case DRUM_ID_KICK: return mkcol(179,29,5);
		case DRUM_ID_SNARE: return mkcol(255,191,37);
		case DRUM_ID_HIHAT: return mkcol(85,195,235);
		case DRUM_ID_OPEN: return mkcol(255,255,255);
		default: return 0;
	}
}


static void piano_roll_render(struct piano_roll* piano_roll, uint32_t* screen)
{
	float width_in_seconds = 5.0f;
	float offset_in_seconds = 1.5f;

	float bps = piano_roll->song->bpm / 60.0;

	float width_in_beats = width_in_seconds * bps;
	float offset_in_beats = offset_in_seconds * bps;

	float current_beat = piano_roll->time_in_seconds * bps;

	float x0 = (SCREEN_WIDTH * offset_in_seconds) / width_in_seconds;

	// render cursor
	{
		screen_draw_rect(screen, (int)x0-1, 8, 3, 48, mkcol(80,80,80));
	}

	// render bars
	int b0 = (int)floorf(current_beat - offset_in_beats);
	int b1 = b0 + (int)ceilf(width_in_beats) + 1;
	float beat_width = SCREEN_WIDTH / width_in_beats;
	for (int b = b0; b <= b1; b++) {
		for (int sub = 0; sub < 4; sub++) {
			float bf = (float)b + (float)sub * 0.25f;

			uint32_t color = 0;
			if (sub == 0) {
				if ((b % piano_roll->song->time_signature) == 0) {
					color = mkcol(255,255,255);
				} else {
					color = mkcol(200,200,0);
				}
			} else {
				color = mkcol(80,80,80);
			}
			float x = x0 + (bf - current_beat) * beat_width;
			screen_draw_rect(screen, (int)x, 16, 1, 32, color);
		}
	}

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
			screen_draw_rect(screen, (int)x-1, 16+drum_id*8, 3, 2, drum_color(drum_id));
			screen_draw_rect(screen, (int)x-1, 16+drum_id*8+6, 3, 2, drum_color(drum_id));
		}
	}

	// render played notes
	float second_width = (float)SCREEN_WIDTH / width_in_seconds;
	for (int i = 0; i < MAX_PLAYED_NOTES; i++) {
		struct played_note* note = &piano_roll->played_notes[i];
		if (note->time_in_seconds <= 0.0) continue;
		float dt = note->time_in_seconds - piano_roll->time_in_seconds;
		float x = x0 + dt * second_width;
		screen_draw_rect(screen, (int)x-2, 16+note->drum_id*8+2, 5, 4, drum_color(note->drum_id));
	}

	// render gauge
	{
		int dx = piano_roll->gauge * SCREEN_WIDTH;
		if (dx < 0) dx = 0;
		if (dx >= SCREEN_WIDTH) dx = SCREEN_WIDTH;
		screen_draw_rect(screen, 0, 200, dx, 8, mkcol(0,255,0));
	}
}

static void present_screen(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, uint32_t* screen)
{
	//SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	//SDL_RenderClear(renderer);

	SDL_UpdateTexture(texture, NULL, screen, SCREEN_WIDTH * sizeof(uint32_t));

	int window_width;
	int window_height;
	SDL_GetWindowSize(window, &window_width, &window_height);

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
	float dt_accum;
	struct zombie* zombies;
	int spawn_counter;
	int ticks;
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

	rng_seed(&zd->rng, 420);
}

static void zombie_director_update(struct zombie_director* zd, struct piano_roll* piano_roll, float dt)
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

static void zombie_director_render(struct zombie_director* zd, uint32_t* screen)
{
	qsort(zd->zombies, MAX_ZOMBIES, sizeof(struct zombie), zombie_y_sort);
	int anim_offset = 82;
	for (int i = 0; i < MAX_ZOMBIES; i++) {
		struct zombie* z = &zd->zombies[i];
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
};

static void drummer_init(struct drummer* drummer)
{
	memset(drummer, 0, sizeof(*drummer));

	// drummerp.png PNG 150x240 150x240+0+0 8-bit sRGB 26c 1.77KB 0.000u 0:00.000
	img_load(&drummer->img, "drummerp.png");
	ASSERT(drummer->img.width == 150);
	ASSERT(drummer->img.height == 240);

	drummer->x = 45;
	drummer->y = 112;
	drummer->kill_dx = 14;
}

static void drummer_update(struct drummer* drummer, int drum_control, struct zombie_director* zombie_director, float dt)
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

static void drummer_render(struct drummer* drummer, uint32_t* screen)
{
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

	// state
	int gib;
	int dead;
	float dt_accum;
};

static void player_init(struct player* player, const char* asset, int width, int height, int x, int y, int anim_offset, int frames, int kill_dx)
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
}

static int player_effective_x(struct player* player)
{
	return player->x + player->kill_dx;
}

static void player_update(struct player* p, struct zombie_director* zombie_director, float dt)
{
	if (p->dead) return;

	p->dt_accum += dt;

	float tick_time = 0.05f;
	int gib_duration = 7;

	while (p->dt_accum > 0) {
		if (p->gib) {
			if (p->gib > gib_duration) {
				p->dead = 1;
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

static void player_render(struct player* player, uint32_t* screen, int step)
{
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
	piano_roll_init(&piano_roll, &song_data_test);

	uint32_t feedback_cursor = 0;

	uint8_t drum_control_keymap[128];
	memset(drum_control_keymap, 0, 128);

	// XXX hard-coded for now, TODO config, TODO prefs (SDL_GetPrefPath())
	drum_control_keymap['z'] = DRUM_CONTROL_KICK;
	drum_control_keymap['x'] = DRUM_CONTROL_KICK;
	drum_control_keymap['c'] = DRUM_CONTROL_SNARE;
	drum_control_keymap['v'] = DRUM_CONTROL_SNARE;
	drum_control_keymap['m'] = DRUM_CONTROL_HIHAT;
	drum_control_keymap[','] = DRUM_CONTROL_HIHAT;
	drum_control_keymap['.'] = DRUM_CONTROL_OPEN;

	struct img bg_img;
	// assets/background.png PNG 384x216 384x216+0+0 8-bit sRGB 256c 2.22KB 0.000u 0:00.000
	img_load(&bg_img, "background.png");
	ASSERT(bg_img.width == SCREEN_WIDTH);
	ASSERT(bg_img.height == SCREEN_HEIGHT);

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
		42);

	struct player guitar_player;
	player_init(
		&guitar_player,
		"guitarp.png",
		65, 534,
		120, 96,
		89,
		6,
		34);

	struct zombie_director zombie_director;
	zombie_director_init(&zombie_director);

	int drum_control_cooldown[DRUM_ID_MAX] = {0};

	uint32_t* screen = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
	AN(screen);

	int exiting = 0;
	while (!exiting) {
		SDL_Event e;
		uint32_t drum_control = 0;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) exiting = 1;
			if (e.type == SDL_KEYDOWN) {
				if (e.key.keysym.sym == SDLK_ESCAPE) {
					exiting = 1;
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
			audio.drum_control |= drum_control;
		}
		audio_unlock(&audio);


		memset(screen, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));


		uint32_t cool_drum_control = 0;
		for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
			int mask = 1<<drum_id;
			if (drum_control & mask) {
				int cooldown = drum_id == DRUM_ID_OPEN ? mode.refresh_rate * 2: mode.refresh_rate / 10;
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

		if (step & 4) {
			cool_drum_control |= DRUM_CONTROL_HEAD;
		}

		piano_roll_update_position(&piano_roll, &audio, audio_position);
		piano_roll_update_gauge(&piano_roll);

		float dt = 1.0f / (float)mode.refresh_rate;

		zombie_director_update(&zombie_director, &piano_roll, dt);
		drummer_update(&drummer, cool_drum_control, &zombie_director, dt);
		player_update(&bass_player, &zombie_director, dt);
		player_update(&guitar_player, &zombie_director, dt);

		memcpy(screen, bg_img.data, SCREEN_WIDTH*SCREEN_HEIGHT*sizeof(uint32_t));

		drummer_render(&drummer, screen);

		player_render(&bass_player, screen, step);
		player_render(&guitar_player, screen, step);
		piano_roll_render(&piano_roll, screen);
		zombie_director_render(&zombie_director, screen);

		present_screen(window, renderer, texture, screen);
	}

	audio_quit(&audio);

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	return EXIT_SUCCESS;
}
