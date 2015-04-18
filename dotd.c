#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "platform.h"

#include "a.h"
#include "m.h"

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

#if 0
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
#endif

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


#define DS_K0 (0)
#define DS_K1 (1)
#define DS_K2 (2)
#define DS_S0 (3)
#define DS_S1 (4)
#define DS_S2 (5)
#define DS_H0 (6)
#define DS_H1 (7)
#define DS_H2 (8)
#define DS_O0 (9)
#define DS_O1 (10)
#define DS_O2 (11)
#define DS_MAX (12)

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
	struct sample samples[DS_MAX];
};


static void drum_samples_init(struct drum_samples* ds)
{
	struct alist {
		int i;
		const char* file;
	} alist[] = {
		{DS_K0, "k0.wav"},
		{DS_K1, "k1.wav"},
		{DS_K2, "k2.wav"},
		{DS_S0, "s0.wav"},
		{DS_S1, "s1.wav"},
		{DS_S2, "s2.wav"},
		{DS_H0, "h0.wav"},
		{DS_H1, "h1.wav"},
		{DS_H2, "h2.wav"},
		{DS_O0, "o0.wav"},
		{DS_O1, "o1.wav"},
		{DS_O2, "o2.wav"},
		{-1, NULL}
	};

	struct alist* ae = alist;
	while (ae->file) {
		struct sample* sample = &ds->samples[ae->i];
		sample_load(sample, ae->file);
		ae++;
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
	want.samples = 256;
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
	struct played_note played_notes[MAX_PLAYED_NOTES];
	struct song* song;
};


static void piano_roll_update_position(struct piano_roll* p, struct audio* audio, uint32_t audio_position)
{
	p->time_in_seconds = audio_position_to_seconds(audio, audio_position);
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
	}
}

static void piano_roll_init(struct piano_roll* p, struct song* song)
{
	memset(p, 0, sizeof(*p));
	p->song = song;
}

#if 0
static void renderer_set_drum_color(SDL_Renderer* renderer, int drum_id)
{
	switch (drum_id) {
		case 0:
			SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
			break;
		case 1:
			SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
			break;
		case 2:
			SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
			break;
	}
}

static void piano_roll_render(struct piano_roll* piano_roll, SDL_Renderer* renderer, struct stage* stage)
{
	float width_in_seconds = 5.0f;
	float offset_in_seconds = 1.5f;

	float bps = piano_roll->song->bpm / 60.0;

	float width_in_beats = width_in_seconds * bps;
	float offset_in_beats = offset_in_seconds * bps;

	float current_beat = piano_roll->time_in_seconds * bps;

	float x0 = (stage->width * offset_in_seconds) / width_in_seconds;

	// render cursor
	{
		SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
		SDL_Rect rect;
		rect.x = (int)x0-1;
		rect.y = 0;
		rect.w = 4;
		rect.h = 128+32;
		SDL_RenderFillRect(renderer, &rect);
	}

	// render bars
	int b0 = (int)floorf(current_beat - offset_in_beats);
	int b1 = b0 + (int)ceilf(width_in_beats) + 1;
	float beat_width = stage->width / width_in_beats;
	for (int b = b0; b <= b1; b++) {
		for (int sub = 0; sub < 4; sub++) {
			float bf = (float)b + (float)sub * 0.25f;

			if (sub == 0) {
				if ((b % piano_roll->song->time_signature) == 0) {
					SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
				} else {
					SDL_SetRenderDrawColor(renderer, 200, 200, 0, 255);
				}
			} else {
				SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
			}

			float x = x0 + (bf - current_beat) * beat_width;

			SDL_Rect rect;
			rect.x = (int)x;
			rect.y = 16;
			rect.w = 2;
			rect.h = 128;
			SDL_RenderFillRect(renderer, &rect);
		}
	}

	// render song
	int s0 = (int)floorf((current_beat - offset_in_beats) * piano_roll->song->lps);
	int s1 = s0 + (int)ceil(width_in_beats * piano_roll->song->lps)+1;
	float step_width = (stage->width / width_in_beats) / piano_roll->song->lps;
	float current_step = current_beat * piano_roll->song->lps;
	for (int s = s0; s <= s1; s++) {
		if (s < 0 || s >= piano_roll->song->length) continue;
		float x = x0 + ((float)s - current_step) * step_width;
		int dctl = piano_roll->song->drums[s];

		for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
			int mask = 1<<drum_id;
			if (!(dctl & mask)) continue;
			renderer_set_drum_color(renderer, drum_id);
			SDL_Rect rect;
			rect.x = (int)x-8;
			rect.y = 24 + drum_id * 48;
			rect.w = 8;
			rect.h = 16;
			SDL_RenderFillRect(renderer, &rect);
		}
	}

	// render notes
	float second_width = (float)stage->width / width_in_seconds;
	for (int i = 0; i < MAX_PLAYED_NOTES; i++) {
		struct played_note* note = &piano_roll->played_notes[i];
		if (note->time_in_seconds <= 0.0) continue;
		float dt = note->time_in_seconds - piano_roll->time_in_seconds;
		float x = x0 + dt * second_width;
		renderer_set_drum_color(renderer, note->drum_id);
		SDL_Rect rect;
		rect.x = (int)x-16;
		rect.y = 16 + note->drum_id * 48;
		rect.w = 16;
		rect.h = 32;
		SDL_RenderFillRect(renderer, &rect);
	}
}
#endif


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

static void draw_drummer(uint32_t* screen, struct img* img, uint32_t drum_control)
{
	//screen_draw_img(screen, img, 0, 0, 0, 0, 100, 100);
	#if 0
	// src offset, src dim - draw offset
	histick 13,96 21x7 - 14,28
	hihat 4,110 20x7 - 24,33
	head 2,29 17x21 - 1,0
	kick 1,1 30x26 - 0,39
	snare 3,61 34x23 - 3,18
	static 40,1 26x26 - 27,39
	#endif

	int x = 64;
	int y = 64;
	int actoff = img->height >> 1;

	// histick
	screen_draw_img(screen, img, 13, 96 + ((drum_control & DRUM_CONTROL_HIHAT) ? actoff : 0), x+14, y+28, 21, 7);

	// hihat
	screen_draw_img(screen, img, 4, 110 + ((drum_control & DRUM_CONTROL_OPEN) ? actoff : 0), x+24, y+33, 20, 7);

	// head
	screen_draw_img(screen, img, 2, 29 + ((drum_control & DRUM_CONTROL_HEAD) ? actoff : 0), x+1, y+0, 17, 21);

	// kick
	screen_draw_img(screen, img, 1, 1 + ((drum_control & DRUM_CONTROL_KICK) ? actoff : 0), x+0, y+39, 30, 26);

	// snare
	screen_draw_img(screen, img, 3, 61 + ((drum_control & DRUM_CONTROL_SNARE) ? actoff : 0), x+3, y+18, 34, 23);

	// static
	screen_draw_img(screen, img, 40, 1, x+27, y+39, 26, 26);

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
	drum_control_keymap[','] = DRUM_CONTROL_HIHAT;
	drum_control_keymap['.'] = DRUM_CONTROL_HIHAT;
	drum_control_keymap['/'] = DRUM_CONTROL_OPEN;


	struct img drummer_img;
	// drummerp.png PNG 150x240 150x240+0+0 8-bit sRGB 26c 1.77KB 0.000u 0:00.000
	img_load(&drummer_img, "drummerp.png");
	ASSERT(drummer_img.width == 150);
	ASSERT(drummer_img.height == 240);

	int drum_control_cooldown[DRUM_ID_MAX] = {0};
	int drum_control_cooldown_duration = 4; // FIXME set from framerate


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

		piano_roll_update_position(&piano_roll, &audio, audio_position);
		#if 0
		piano_roll_render(&piano_roll, renderer, &stage);
		#endif

		uint32_t cool_drum_control = 0;
		for (int drum_id = 0; drum_id < DRUM_ID_MAX; drum_id++) {
			int mask = 1<<drum_id;
			if (drum_control & mask) {
				drum_control_cooldown[drum_id] = drum_control_cooldown_duration;
			}
			if (drum_control_cooldown[drum_id] > 0) {
				cool_drum_control |= mask;
				drum_control_cooldown[drum_id]--;
			}
		}
		if (((int)(audio_position_to_seconds(&audio, audio_position) * 2))&1) {
			// FIXME set from bpm
			cool_drum_control |= DRUM_CONTROL_HEAD;
		}
		draw_drummer(screen, &drummer_img, cool_drum_control);

		present_screen(window, renderer, texture, screen);
	}

	audio_quit(&audio);

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	return EXIT_SUCCESS;
}
