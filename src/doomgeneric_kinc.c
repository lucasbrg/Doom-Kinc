//------------------------------------------------------------------------------
//  doomgeneric_kinc.c
//
//  This is all the kinc-backend-specific code
//------------------------------------------------------------------------------
#include "d_event.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "i_video.h"
#include "m_argv.h"
#include "sounds.h"
#include "w_wad.h"

#include <assert.h>
#include <math.h> // round
#include <string.h>

#include "kinc/audio2/audio.h"
#include "kinc/graphics1/graphics.h"
#include "kinc/input/keyboard.h"
#include "kinc/input/mouse.h"
#include "kinc/io/filereader.h"
#include "kinc/system.h"

#define MUS_IMPLEMENTATION
#include "mus.h"
#define TSF_IMPLEMENTATION
#include "tsf.h"

// in m_menu.c
extern boolean menuactive;

void D_DoomMain(void);
void D_DoomLoop(void);
void D_DoomFrame(void);
void dg_Create();

#define KEY_QUEUE_SIZE (32)
#define MAXSAMPLECOUNT (4096)
#define NUM_CHANNELS (8)
#define MAX_WAD_SIZE (16 * 1024 * 1024)
#define MAX_SOUNDFONT_SIZE (2 * 1024 * 1024)

typedef struct {
	uint8_t key_code;
	bool pressed;
} key_state_t;

typedef struct {
	uint8_t *cur_ptr;
	uint8_t *end_ptr;
	int sfxid;
	int handle;
	int leftvol;
	int rightvol;
} snd_channel_t;

typedef enum {
	DATA_STATE_LOADING,
	DATA_STATE_VALID,
	DATA_STATE_FAILED,
} data_state_t;

static struct {
	uint32_t frames_per_tick; // number of frames per game tick
	uint32_t frame_tick_counter;
	struct {
		key_state_t key_queue[KEY_QUEUE_SIZE];
		uint32_t key_write_index;
		uint32_t key_read_index;
		uint32_t mouse_button_state;
		uint32_t delayed_mouse_button_up;
		bool held_alt;
		bool wasd_enabled;
	} input;
	struct {
		bool use_sfx_prefix;
		uint16_t cur_sfx_handle;
		snd_channel_t channels[NUM_CHANNELS];
		uint32_t resample_outhz;
		uint32_t resample_inhz;
		uint32_t resample_accum;
		int lengths[NUMSFX]; // length in bytes/samples of sound effects
	} sound;
	struct {
		tsf *sound_font;
		void *cur_song_data;
		int cur_song_len;
		int volume;
		mus_t *mus;
		bool reset;
		int leftover;
	} music;
	struct {
		struct {
			data_state_t state;
			size_t size;
			uint8_t buf[MAX_WAD_SIZE];
		} wad;
		struct {
			data_state_t state;
			size_t size;
			uint8_t buf[MAX_SOUNDFONT_SIZE];
		} sf;
	} data;
} app;

static void snd_mix(int, float *);
static void mus_mix(int, float *);
static void audio_callback(kinc_a2_buffer_t *buffer, int samples) {
	const int num_frames = samples / 2;
	if (num_frames > 0) {
		assert(num_frames <= MAXSAMPLECOUNT);
		snd_mix(num_frames, (float *)buffer->data);
		mus_mix(num_frames, (float *)buffer->data);

		buffer->read_location = 0;
	}
}
static void update_game_audio(void) {
	kinc_a2_update();
}

static void draw_game_frame(void) {

	uint32_t *palette = (uint32_t *)I_GetPalette();
	byte *buffer = I_VideoBuffer;

	kinc_g1_begin();

	// DRAW SCREEN
	for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; ++i) {
		kinc_internal_g1_image[i] = palette[buffer[i]];
	}

	kinc_g1_end();
}

void frame(void) {

	// compute frames-per-tick to get us close to the ideal 35 Hz game tick
	// but without skipping ticks
	static double time_current = 0;
	static double time_last = 0;

	time_last = time_current;
	time_current = kinc_time();

	double frame_time_ms = (time_current - time_last) * 1000.0;
	if (frame_time_ms > 40.0) {
		// prevent overly long frames (for instance when in debugger)
		frame_time_ms = 40.0;
	}
	const double tick_time_ms = 1000.0 / 35.0;
	app.frames_per_tick = (uint32_t)round(tick_time_ms / frame_time_ms);

	if (++app.frame_tick_counter >= app.frames_per_tick) {
		app.frame_tick_counter = 0;

		D_DoomFrame();
		// this prevents that very short mouse button taps on touchpads are not deteced
		if (app.input.delayed_mouse_button_up != 0) {
			app.input.mouse_button_state &= ~app.input.delayed_mouse_button_up;
			app.input.delayed_mouse_button_up = 0;
			D_PostEvent(&(event_t){.type = ev_mouse, .data1 = app.input.mouse_button_state});
		}

		if (menuactive) {
			if (kinc_mouse_is_locked()) {
				kinc_mouse_unlock();
			}

			app.input.wasd_enabled = false;
		}
		else {
			if (!kinc_mouse_is_locked()) {
				kinc_mouse_lock(0);
			}
			app.input.wasd_enabled = true;
		}
	}
	update_game_audio();
	draw_game_frame();
}

static void push_key(uint8_t key_code, bool pressed) {
	if (key_code != 0) {
		assert(app.input.key_write_index < KEY_QUEUE_SIZE);
		app.input.key_queue[app.input.key_write_index] = (key_state_t){.key_code = key_code, .pressed = pressed};
		app.input.key_write_index = (app.input.key_write_index + 1) % KEY_QUEUE_SIZE;
	}
}

static key_state_t pull_key(void) {
	if (app.input.key_read_index == app.input.key_write_index) {
		return (key_state_t){0};
	}
	else {
		assert(app.input.key_read_index < KEY_QUEUE_SIZE);
		key_state_t res = app.input.key_queue[app.input.key_read_index];
		app.input.key_read_index = (app.input.key_read_index + 1) % KEY_QUEUE_SIZE;
		return res;
	}
}

// originally in i_video.c
static int AccelerateMouse(int val) {
	if (val < 0) {
		return -AccelerateMouse(-val);
	}
// Win32 hack to speed up mouse
#ifdef _WIN32
	val *= 4;
#endif
	if (val > mouse_threshold) {
		return (int)((val - mouse_threshold) * mouse_acceleration + mouse_threshold);
	}
	else {
		return val;
	}
}

void on_background(void) {
	// clear all input when window loses focus
	push_key(KEY_UPARROW, false);
	push_key(KEY_DOWNARROW, false);
	push_key(KEY_LEFTARROW, false);
	push_key(KEY_RIGHTARROW, false);
	push_key(KEY_STRAFE_L, false);
	push_key(KEY_STRAFE_R, false);
	push_key(KEY_FIRE, false);
	push_key(KEY_USE, false);
	push_key(KEY_TAB, false);
	push_key(KEY_RSHIFT, false);
	push_key(KEY_ESCAPE, false);
	push_key(KEY_ENTER, false);
	push_key('1', false);
	push_key('2', false);
	push_key('3', false);
	push_key('4', false);
	push_key('5', false);
	push_key('6', false);
	push_key('7', false);
}

void on_key(int key_code, bool pressed) {
	switch (key_code) {
	case KINC_KEY_W:
		if (app.input.wasd_enabled) {
			push_key(KEY_UPARROW, pressed);
		}
		else {
			push_key('w', pressed);
		}
		break;
	case KINC_KEY_UP:
		push_key(KEY_UPARROW, pressed);
		break;
	case KINC_KEY_S:
		if (app.input.wasd_enabled) {
			push_key(KEY_DOWNARROW, pressed);
		}
		else {
			push_key('s', pressed);
		}
		break;
	case KINC_KEY_DOWN:
		push_key(KEY_DOWNARROW, pressed);
		break;
	case KINC_KEY_LEFT:
		if (pressed) {
			if (app.input.held_alt) {
				push_key(KEY_STRAFE_L, true);
			}
			else {
				push_key(KEY_LEFTARROW, true);
			}
		}
		else {
			push_key(KEY_STRAFE_L, false);
			push_key(KEY_LEFTARROW, false);
		}
		break;
	case KINC_KEY_RIGHT:
		if (pressed) {
			if (app.input.held_alt) {
				push_key(KEY_STRAFE_R, true);
			}
			else {
				push_key(KEY_RIGHTARROW, true);
			}
		}
		else {
			push_key(KEY_STRAFE_R, false);
			push_key(KEY_RIGHTARROW, false);
		}
		break;
	case KINC_KEY_A:
		if (app.input.wasd_enabled) {
			push_key(KEY_STRAFE_L, pressed);
		}
		else {
			push_key('a', pressed);
		}
		break;
	case KINC_KEY_D:
		if (app.input.wasd_enabled) {
			push_key(KEY_STRAFE_R, pressed);
		}
		else {
			push_key('d', pressed);
		}
		break;
	case KINC_KEY_SPACE:
		push_key(KEY_USE, pressed);
		break;
	case KINC_KEY_CONTROL:
		push_key(KEY_FIRE, pressed);
		break;
	case KINC_KEY_ESCAPE:
		push_key(KEY_ESCAPE, pressed);
		break;
	case KINC_KEY_RETURN:
		push_key(KEY_ENTER, pressed);
		break;
	case KINC_KEY_TAB:
		push_key(KEY_TAB, pressed);
		break;
	case KINC_KEY_SHIFT:
		push_key(KEY_RSHIFT, pressed);
		break;
	case KINC_KEY_BACKSPACE:
		push_key(KEY_BACKSPACE, pressed);
		break;
	case KINC_KEY_ALT:
		app.input.held_alt = pressed;
		break;
	case KINC_KEY_1:
		push_key('1', pressed);
		break;
	case KINC_KEY_2:
		push_key('2', pressed);
		break;
	case KINC_KEY_3:
		push_key('3', pressed);
		break;
	case KINC_KEY_4:
		push_key('4', pressed);
		break;
	case KINC_KEY_5:
		push_key('5', pressed);
		break;
	case KINC_KEY_6:
		push_key('6', pressed);
		break;
	case KINC_KEY_7:
		push_key('7', pressed);
		break;
	default:
		if (key_code >= 65 && key_code <= 90) {
			push_key(key_code, pressed);
		}
		break;
	}
}

void on_key_down(int key_code) {
	on_key(key_code, true);
}
void on_key_up(int key_code) {
	on_key(key_code, false);
}

void mouse_press(int window, int button, int x, int y) {
	app.input.mouse_button_state |= (1 << button);
	D_PostEvent(&(event_t){
	    .type = ev_mouse,
	    .data1 = app.input.mouse_button_state,
	});
}

void mouse_release(int window, int button, int x, int y) {
	// delay mouse up to the next frame so that short
	// taps on touch pads are registered
	app.input.delayed_mouse_button_up |= (1 << button);
}

void mouse_move(int window, int x, int y, int movement_x, int movement_y) {
	D_PostEvent(&(event_t){
	    .type = ev_mouse,
	    .data1 = app.input.mouse_button_state,
	    .data2 = AccelerateMouse(movement_x),
	});
}

void init(void) {
	kinc_init("DOOM-Kinc", SCREENWIDTH * 4, SCREENHEIGHT * 4, NULL, NULL);
	kinc_g1_init(SCREENWIDTH, SCREENHEIGHT);
	kinc_keyboard_set_key_down_callback(&on_key_down);
	kinc_keyboard_set_key_up_callback(&on_key_up);
	kinc_mouse_set_press_callback(&mouse_press);
	kinc_mouse_set_release_callback(&mouse_release);
	kinc_mouse_set_move_callback(&mouse_move);
	kinc_set_background_callback(&on_background);

	kinc_a2_init();
	kinc_a2_set_callback(&audio_callback);

	kinc_file_reader_t reader;

	if (kinc_file_reader_open(&reader, "DOOM1.WAD", KINC_FILE_TYPE_ASSET)) {
		app.data.wad.size = kinc_file_reader_size(&reader);
		app.data.wad.state = DATA_STATE_VALID;
		kinc_file_reader_read(&reader, app.data.wad.buf, app.data.wad.size);
		kinc_file_reader_close(&reader);
	}
	if (kinc_file_reader_open(&reader, "AweROMGM.sf2", KINC_FILE_TYPE_ASSET)) {
		app.data.sf.size = kinc_file_reader_size(&reader);
		app.data.sf.state = DATA_STATE_VALID;
		kinc_file_reader_read(&reader, app.data.sf.buf, app.data.sf.size);
		kinc_file_reader_close(&reader);
	}

	kinc_set_update_callback(&frame);

	dg_Create();
	// D_DoomMain() without the trailing call to D_DoomLoop()
	D_DoomMain();
}

void cleanup(void) {
	// tsf_close(app.music.sound_font);
	// saudio_shutdown();
	// sfetch_shutdown();
	// sdtx_shutdown();
	// sg_shutdown();
}

int kickstart(int argc, char *argv[]) {
	(void)argc;
	(void)argv;
	static char *args[] = {"doom", "-iwad", "DOOM1.WAD"};
	myargc = 3;
	myargv = args;

	init();
	kinc_start();

	cleanup();

	return 0;
}

//== DoomGeneric backend callbacks =============================================

// Note that some of those are empty, because they only make sense
// in an "own the game loop" scenario, not in a frame-callback scenario.

void DG_Init(void) {
	// initialize sound font
	assert(app.data.sf.size > 0);
	app.music.sound_font = tsf_load_memory(app.data.sf.buf, app.data.sf.size);
	tsf_set_output(app.music.sound_font, TSF_STEREO_INTERLEAVED, kinc_a2_samples_per_second, 0);
}

void DG_DrawFrame(void) {}

void DG_SetWindowTitle(const char *title) {
	// window title changes ignored
	(void)title;
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
	key_state_t key_state = pull_key();
	if (key_state.key_code != 0) {
		*doomKey = key_state.key_code;
		*pressed = key_state.pressed ? 1 : 0;
		return 1;
	}
	else {
		// no key available
		return 0;
	}
}

// the sleep function is used in blocking wait loops, those don't
// work in a browser environment anyway, inject an assert instead
// so we easily find all those wait loops
void DG_SleepMs(uint32_t ms) {
	assert(false && "DG_SleepMS called!\n");
}

// NOTE: game loop timing is done entirely through the frame callback,
// the DG_GetTicksMs() function is now only called from the
// menu input handling code for mouse and joystick, where timing
// doesn't matter.
uint32_t DG_GetTicksMs(void) {
	return 0;
}

//== FILE SYSTEM OVERRIDE ======================================================
#include "m_misc.h"
#include "memio.h"
#include "w_file.h"
#include "z_zone.h"

typedef struct {
	wad_file_t wad;
	MEMFILE *fstream;
} memio_wad_file_t;

// at end of file!
extern wad_file_class_t memio_wad_file;

static wad_file_t *memio_OpenFile(char *path) {
	if (0 != strcmp(path, "DOOM1.WAD")) {
		return 0;
	}
	assert(app.data.wad.size > 0);
	MEMFILE *fstream = mem_fopen_read(app.data.wad.buf, app.data.wad.size);
	if (fstream == 0) {
		return 0;
	}

	memio_wad_file_t *result = Z_Malloc(sizeof(memio_wad_file_t), PU_STATIC, 0);
	result->wad.file_class = &memio_wad_file;
	result->wad.mapped = NULL;
	result->wad.length = app.data.wad.size;
	result->fstream = fstream;

	return &result->wad;
}

static void memio_CloseFile(wad_file_t *wad) {
	memio_wad_file_t *memio_wad = (memio_wad_file_t *)wad;
	mem_fclose(memio_wad->fstream);
	Z_Free(memio_wad);
}

static size_t memio_Read(wad_file_t *wad, uint32_t offset, void *buffer, size_t buffer_len) {
	memio_wad_file_t *memio_wad = (memio_wad_file_t *)wad;
	mem_fseek(memio_wad->fstream, offset, MEM_SEEK_SET);
	return mem_fread(buffer, 1, buffer_len, memio_wad->fstream);
}

wad_file_class_t memio_wad_file = {
    .OpenFile = memio_OpenFile,
    .CloseFile = memio_CloseFile,
    .Read = memio_Read,
};

/*== SOUND SUPPORT ===========================================================*/

// see https://github.com/mattiasgustavsson/doom-crt/blob/main/linuxdoom-1.10/i_sound.c

// helper function to load sound data from WAD lump
static void *snd_getsfx(const char *sfxname, int *len) {
	char name[20];
	snprintf(name, sizeof(name), "ds%s", sfxname);
	int sfxlump;
	if (W_CheckNumForName(name) == -1) {
		sfxlump = W_GetNumForName("dspistol");
	}
	else {
		sfxlump = W_GetNumForName(name);
	}
	const int size = W_LumpLength(sfxlump);
	assert(size > 8);

	uint8_t *sfx = W_CacheLumpNum(sfxlump, PU_STATIC);
	*len = size - 8;
	return sfx + 8;
}

// This function adds a sound to the list of currently active sounds,
// which is maintained as a given number (eight, usually) of internal channels.
// Returns a handle.
//
static int snd_addsfx(int sfxid, int slot, int volume, int separation) {
	assert((slot >= 0) && (slot < NUM_CHANNELS));
	assert((sfxid >= 0) && (sfxid < NUMSFX));

	/* SOKOL CHANGE: this doesn't seem to be necessary unless the
	   sound playback is in an extern process (like fbDoom's sndserv)

	// Chainsaw troubles.
	// Play these sound effects only one at a time.
	if ((sfxid == sfx_sawup) ||
	    (sfxid == sfx_sawidl) ||
	    (sfxid == sfx_sawful) ||
	    (sfxid == sfx_sawhit) ||
	    (sfxid == sfx_stnmov) ||
	    (sfxid == sfx_pistol))
	{
	    for (int i = 0; i < NUM_CHANNELS; i++) {
	        if (app.sound.channels[i].sfxid == sfxid) {
	            // reset
	            app.sound.channels[i] = (snd_channel_t){0};
	            // we are sure that if, there will be only one
	            break;
	        }
	    }
	}
	*/
	app.sound.channels[slot].sfxid = sfxid;
	app.sound.cur_sfx_handle += 1;
	// on wraparound skip the 'invalid handle' 0
	if (app.sound.cur_sfx_handle == 0) {
		app.sound.cur_sfx_handle = 1;
	}
	app.sound.channels[slot].handle = (int)app.sound.cur_sfx_handle;
	app.sound.channels[slot].cur_ptr = S_sfx[sfxid].driver_data;
	app.sound.channels[slot].end_ptr = app.sound.channels[slot].cur_ptr + app.sound.lengths[sfxid];

	// Separation, that is, orientation/stereo. range is: 1 - 256
	separation += 1;

	// Per left/right channel.
	//  x^2 seperation,
	//  adjust volume properly.
	int left_sep = separation + 1;
	int leftvol = volume - ((volume * left_sep * left_sep) >> 16);
	assert((leftvol >= 0) && (leftvol <= 127));
	int right_sep = separation - 256;
	int rightvol = volume - ((volume * right_sep * right_sep) >> 16);
	assert((rightvol >= 0) && (rightvol <= 127));

	app.sound.channels[slot].leftvol = leftvol;
	app.sound.channels[slot].rightvol = rightvol;

	return app.sound.channels[slot].handle;
}

static float snd_clampf(float val, float maxval, float minval) {
	if (val > maxval) {
		return maxval;
	}
	else if (val < minval) {
		return minval;
	}
	else {
		return val;
	}
}

// mix active sound channels into the mixing buffer
static void snd_mix(int num_frames, float *buffer) {

	float cur_left_sample = 0.0f;
	float cur_right_sample = 0.0f;

	for (int frame_index = 0; frame_index < num_frames; frame_index++) {
		// downsampling: compute new left/right sample?
		if (app.sound.resample_accum >= app.sound.resample_outhz) {
			app.sound.resample_accum -= app.sound.resample_outhz;
			int dl = 0;
			int dr = 0;
			for (int slot = 0; slot < NUM_CHANNELS; slot++) {
				snd_channel_t *chn = &app.sound.channels[slot];
				if (chn->cur_ptr) {
					int sample = ((int)(*chn->cur_ptr++)) - 128;
					dl += sample * chn->leftvol;
					dr += sample * chn->rightvol;
					// sound effect done?
					if (chn->cur_ptr >= chn->end_ptr) {
						*chn = (snd_channel_t){0};
					}
				}
			}
			cur_left_sample = snd_clampf(((float)dl) / 16383.0f, 1.0f, -1.0f);
			cur_right_sample = snd_clampf(((float)dr) / 16383.0f, 1.0f, -1.0f);
		}
		app.sound.resample_accum += app.sound.resample_inhz;

		// write left and right sample values to mix buffer
		buffer[frame_index * 2] = cur_left_sample;
		buffer[frame_index * 2 + 1] = cur_right_sample;
	}
}

static boolean snd_Init(boolean use_sfx_prefix) {
	assert(use_sfx_prefix);
	app.sound.use_sfx_prefix = use_sfx_prefix;
	assert(app.sound.use_sfx_prefix);
	app.sound.resample_outhz = app.sound.resample_accum = kinc_a2_samples_per_second;
	app.sound.resample_inhz = 11025; // sound effect are in 11025Hz
	return true;
}

static void snd_Shutdown(void) {
	// nothing to do here
}

static int snd_GetSfxLumpNum(sfxinfo_t *sfx) {
	char namebuf[20];
	if (app.sound.use_sfx_prefix) {
		M_snprintf(namebuf, sizeof(namebuf), "dp%s", sfx->name);
	}
	else {
		M_StringCopy(namebuf, sfx->name, sizeof(namebuf));
	}
	return W_GetNumForName(namebuf);
}

static void snd_Update(void) {
	// callback at display refresh rate
}

static void snd_UpdateSoundParams(int handle, int vol, int sep) {
	// FIXME
}

// Starts a sound in a particular sound channel.
//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
static int snd_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
	int sfxid = sfxinfo - S_sfx;
	assert((sfxid >= 0) && (sfxid < NUMSFX));
	int handle = snd_addsfx(sfxid, channel, vol, sep);
	return handle;
}

static void snd_StopSound(int handle) {
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (app.sound.channels[i].handle == handle) {
			app.sound.channels[i] = (snd_channel_t){0};
		}
	}
}

static boolean snd_SoundIsPlaying(int handle) {
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (app.sound.channels[i].handle == handle) {
			return true;
		}
	}
	return false;
}

static void snd_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
	for (int i = 0; i < num_sounds; i++) {
		if (0 == sounds[i].link) {
			// load data from WAD file
			sounds[i].driver_data = snd_getsfx(sounds[i].name, &app.sound.lengths[i]);
		}
		else {
			// previously loaded already?
			const int snd_index = sounds[i].link - sounds;
			assert((snd_index >= 0) && (snd_index < NUMSFX));
			sounds[i].driver_data = sounds[i].link->driver_data;
			app.sound.lengths[i] = app.sound.lengths[snd_index];
		}
	}
}

static snddevice_t sound_kinc_devices[] = {
    SNDDEVICE_SB,
};

sound_module_t sound_kinc_module = {
    .sound_devices = sound_kinc_devices,
    .num_sound_devices = arrlen(sound_kinc_devices),
    .Init = snd_Init,
    .Shutdown = snd_Shutdown,
    .GetSfxLumpNum = snd_GetSfxLumpNum,
    .Update = snd_Update,
    .UpdateSoundParams = snd_UpdateSoundParams,
    .StartSound = snd_StartSound,
    .StopSound = snd_StopSound,
    .SoundIsPlaying = snd_SoundIsPlaying,
    .CacheSounds = snd_CacheSounds,
};

/*== MUSIC SUPPORT ===========================================================*/

// see: https://github.com/mattiasgustavsson/doom-crt/blob/f5108fe122fa9c2a334a0ae387d36ddbabc5bf1a/linuxdoom-1.10/i_sound.c#L576
static void mus_mix(int num_frames, float *buffer) {
	mus_t *mus = app.music.mus;
	if (!mus) {
		return;
	}
	tsf *sf = app.music.sound_font;
	assert(sf);
	if (app.music.reset) {
		tsf_reset(sf);
		app.music.reset = false;
	}
	tsf_set_volume(sf, app.music.volume);
	int leftover_from_previous = app.music.leftover;
	int remaining = num_frames;
	float *output = buffer;
	int leftover = 0;
	if (leftover_from_previous > 0) {
		int count = leftover_from_previous;
		if (count > remaining) {
			leftover = count - remaining;
			count = remaining;
		}
		tsf_render_float(sf, output, count, 1);
		remaining -= count;
		output += count * 2;
	}
	if (leftover > 0) {
		app.music.leftover = leftover;
		return;
	}

	while (remaining) {
		mus_event_t ev;
		mus_next_event(app.music.mus, &ev);
		switch (ev.cmd) {
		case MUS_CMD_RELEASE_NOTE:
			tsf_channel_note_off(sf, ev.channel, ev.data.release_note.note);
			break;
		case MUS_CMD_PLAY_NOTE:
			tsf_channel_note_on(sf, ev.channel, ev.data.play_note.note, ev.data.play_note.volume / 127.0f);
			break;
		case MUS_CMD_PITCH_BEND: {
			int pitch_bend = (ev.data.pitch_bend.bend_amount - 128) * 64 + 8192;
			tsf_channel_set_pitchwheel(sf, ev.channel, pitch_bend);
		} break;
		case MUS_CMD_SYSTEM_EVENT:
			switch (ev.data.system_event.event) {
			case MUS_SYSTEM_EVENT_ALL_SOUNDS_OFF:
				tsf_channel_sounds_off_all(sf, ev.channel);
				break;
			case MUS_SYSTEM_EVENT_ALL_NOTES_OFF:
				tsf_channel_note_off_all(sf, ev.channel);
				break;
			case MUS_SYSTEM_EVENT_MONO:
			case MUS_SYSTEM_EVENT_POLY:
				// not supported
				break;
			case MUS_SYSTEM_EVENT_RESET_ALL_CONTROLLERS:
				tsf_channel_midi_control(sf, ev.channel, 121, 0);
				break;
			}
			break;
		case MUS_CMD_CONTROLLER: {
			int value = ev.data.controller.value;
			switch (ev.data.controller.controller) {
			case MUS_CONTROLLER_CHANGE_INSTRUMENT:
				if (ev.channel == 15) {
					tsf_channel_set_presetnumber(sf, 15, 0, 1);
				}
				else {
					tsf_channel_set_presetnumber(sf, ev.channel, value, 0);
				}
				break;
			case MUS_CONTROLLER_BANK_SELECT:
				tsf_channel_set_bank(sf, ev.channel, value);
				break;
			case MUS_CONTROLLER_VOLUME:
				tsf_channel_midi_control(sf, ev.channel, 7, value);
				break;
			case MUS_CONTROLLER_PAN:
				tsf_channel_midi_control(sf, ev.channel, 10, value);
				break;
			case MUS_CONTROLLER_EXPRESSION:
				tsf_channel_midi_control(sf, ev.channel, 11, value);
				break;
			case MUS_CONTROLLER_MODULATION:
			case MUS_CONTROLLER_REVERB_DEPTH:
			case MUS_CONTROLLER_CHORUS_DEPTH:
			case MUS_CONTROLLER_SUSTAIN_PEDAL:
			case MUS_CONTROLLER_SOFT_PEDAL:
				break;
			}
		} break;
		case MUS_CMD_END_OF_MEASURE:
			// not used
			break;
		case MUS_CMD_FINISH:
			mus_restart(mus);
			break;
		case MUS_CMD_RENDER_SAMPLES: {
			int count = ev.data.render_samples.samples_count;
			if (count > remaining) {
				leftover = count - remaining;
				count = remaining;
			}
			tsf_render_float(sf, output, count, 1);
			remaining -= count;
			output += count * 2;
		} break;
		}
	}
	app.music.leftover = leftover;
}

static boolean mus_Init(void) {
	app.music.reset = true;
	app.music.volume = 7;
	return true;
}

static void mus_Shutdown(void) {
	if (app.music.mus) {
		mus_destroy(app.music.mus);
		app.music.mus = 0;
	}
}

static void mus_SetMusicVolume(int volume) {
	app.music.volume = ((float)volume / 64.0f);
}

static void mus_PauseMusic(void) {
	// FIXME
}

static void mus_ResumeMusic(void) {
	// FIXME
}

static void *mus_RegisterSong(void *data, int len) {
	app.music.cur_song_data = data;
	app.music.cur_song_len = len;
	return 0;
}

static void mus_UnRegisterSong(void *handle) {
	app.music.cur_song_data = 0;
	app.music.cur_song_len = 0;
}

static void mus_PlaySong(void *handle, boolean looping) {
	if (app.music.mus) {
		mus_destroy(app.music.mus);
		app.music.mus = 0;
	}
	assert(app.music.cur_song_data);
	assert(app.music.cur_song_len == *(((uint16_t *)app.music.cur_song_data) + 2) + *(((uint16_t *)app.music.cur_song_data) + 3));
	app.music.mus = mus_create(app.music.cur_song_data, app.music.cur_song_len, 0);
	assert(app.music.mus);
	app.music.leftover = 0;
	app.music.reset = true;
}

static void mus_StopSong(void) {
	assert(app.music.mus);
	mus_destroy(app.music.mus);
	app.music.mus = 0;
	app.music.leftover = 0;
	app.music.reset = true;
}

static boolean mus_MusicIsPlaying(void) {
	// never called
	return false;
}

static void mus_Poll(void) {
	// empty, see update_game_audio() instead
}

static snddevice_t music_kinc_devices[] = {
    SNDDEVICE_AWE32,
};

music_module_t music_kinc_module = {
    .sound_devices = music_kinc_devices,
    .num_sound_devices = arrlen(music_kinc_devices),
    .Init = mus_Init,
    .Shutdown = mus_Shutdown,
    .SetMusicVolume = mus_SetMusicVolume,
    .PauseMusic = mus_PauseMusic,
    .ResumeMusic = mus_ResumeMusic,
    .RegisterSong = mus_RegisterSong,
    .UnRegisterSong = mus_UnRegisterSong,
    .PlaySong = mus_PlaySong,
    .StopSong = mus_StopSong,
    .MusicIsPlaying = mus_MusicIsPlaying,
    .Poll = mus_Poll,
};
