#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "platform.h"

#include "a.h"
#include "m.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "stb_vorbis.c"

#include <SDL.h>

#ifndef M_PI
#define M_PI (3.141592653589793)
#endif

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




struct stage {
	int width;
	int height;
	int offset_x;
	int offset_y;
};

static void stage_update(struct stage* stage, SDL_Window* window)
{
	int window_width;
	int window_height;
	SDL_GetWindowSize(window, &window_width, &window_height);

	int aspect_major = 16;
	int aspect_minor = 9;
	
	float window_aspect = (float)window_width / (float)window_height;
	float target_aspect = (float)aspect_major / (float)aspect_minor;

	if (target_aspect > window_aspect) {
		stage->width = window_width;
		stage->height = (window_width * aspect_minor) / aspect_major;
		stage->offset_x = 0;
		stage->offset_y = (window_height - stage->height) / 2;
	} else {
		stage->width = (window_height * aspect_major) / aspect_minor;
		stage->height = window_height;
		stage->offset_x = (window_width - stage->width) / 2;
		stage->offset_y = 0;
	}

	ASSERT(stage->width > 0);
	ASSERT(stage->height > 0);
	ASSERT(stage->offset_x >= 0);
	ASSERT(stage->offset_y >= 0);
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
#define DS_MAX (9)

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
#define DRUM_ID_MAX (3)

#define DRUM_CONTROL_KICK (1<<(DRUM_ID_KICK))
#define DRUM_CONTROL_SNARE (1<<(DRUM_ID_SNARE))
#define DRUM_CONTROL_HIHAT (1<<(DRUM_ID_HIHAT))

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
	for (int i = 0; i < DRUM_ID_MAX; i++) {
		int m = 1<<i;
		if (drum_control & m) {
			struct sample_ctx* ctx = &audio->drum_sample_ctx[i];
			int di = i*3 + rng_uint32(&audio->rng) % 3;
			ctx->sample = &audio->drum_samples.samples[di];
			ctx->position = 0;
			ctx->playing = 1;
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


//#define PIANO_ROLL_WIDTH_IN_BEATS (10.0)


#define MAX_PLAYED_NOTES (256)

struct played_note {
	float time_in_seconds;
	uint32_t drum_id;
};

struct piano_roll {
	int bpm;
	float time_in_seconds;
	int time_signature;
	struct played_note played_notes[MAX_PLAYED_NOTES];
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

static void piano_roll_init(struct piano_roll* p)
{
	memset(p, 0, sizeof(*p));
	p->bpm = 120; // XXX FIXME get from song
	p->time_signature = 4; // 4/4 FIXME get from song
}

static void piano_roll_render(struct piano_roll* piano_roll, SDL_Renderer* renderer, struct stage* stage)
{
	float width_in_seconds = 5.0f;
	float offset_in_seconds = 1.5f;

	float bps = piano_roll->bpm / 60.0;

	float width_in_beats = width_in_seconds * bps;
	float offset_in_beats = offset_in_seconds * bps;

	float current_beat = piano_roll->time_in_seconds * bps;

	int b0 = (int)floorf(current_beat - offset_in_beats);
	int b1 = b0 + (int)ceilf(width_in_beats) + 1;

	float x0 = (stage->width * offset_in_seconds) / width_in_seconds;
	float beat_width = stage->width / width_in_beats;
	float second_width = (float)stage->width / width_in_seconds;

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
	for (int b = b0; b <= b1; b++) {
		for (int sub = 0; sub < 4; sub++) {
			float bf = (float)b + (float)sub * 0.25f;

			if (sub == 0) {
				if ((b % piano_roll->time_signature) == 0) {
					SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
				} else {
					SDL_SetRenderDrawColor(renderer, 200, 200, 0, 255);
				}
			} else {
				SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
			}

			float x = x0 + (bf - current_beat) * beat_width;

			if (x < 0 || x > stage->width) continue;
			SDL_Rect rect;
			rect.x = (int)x;
			rect.y = 16;
			rect.w = 2;
			rect.h = 128;
			SDL_RenderFillRect(renderer, &rect);
		}
	}

	// render notes
	for (int i = 0; i < MAX_PLAYED_NOTES; i++) {
		struct played_note* note = &piano_roll->played_notes[i];
		if (note->time_in_seconds <= 0.0) continue;
		float dt = note->time_in_seconds - piano_roll->time_in_seconds;
		float x = x0 + dt * second_width;
		switch (note->drum_id) {
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
		SDL_Rect rect;
		rect.x = (int)x-16;
		rect.y = 16+32;
		rect.w = 16;
		rect.h = 128-64;
		SDL_RenderFillRect(renderer, &rect);
	}
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

	SDL_Window* window = SDL_CreateWindow(
			"Drums of the Dead",
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			0, 0,
			SDL_WINDOW_FULLSCREEN_DESKTOP);
	SAN(window);

	SDL_Renderer* renderer = SDL_CreateRenderer(
			window,
			-1, 
			SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SAN(renderer);

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);

	struct audio audio;
	audio_init(&audio);

	struct piano_roll piano_roll;
	piano_roll_init(&piano_roll);

	uint32_t feedback_cursor = 0;

	uint8_t drum_control_keymap[128];
	memset(drum_control_keymap, 0, 128);

	// XXX hard-coded for now, TODO config, TODO prefs (SDL_GetPrefPath())
	drum_control_keymap['z'] = DRUM_CONTROL_KICK;
	drum_control_keymap['x'] = DRUM_CONTROL_KICK;
	drum_control_keymap['c'] = DRUM_CONTROL_SNARE;
	drum_control_keymap['v'] = DRUM_CONTROL_SNARE;
	drum_control_keymap['.'] = DRUM_CONTROL_HIHAT;
	drum_control_keymap['/'] = DRUM_CONTROL_HIHAT;

	struct stage stage;

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

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
		SDL_RenderClear(renderer);

		stage_update(&stage, window);

		piano_roll_update_position(&piano_roll, &audio, audio_position);
		piano_roll_render(&piano_roll, renderer, &stage);

		SDL_RenderPresent(renderer);
	}

	audio_quit(&audio);

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	return EXIT_SUCCESS;
}
