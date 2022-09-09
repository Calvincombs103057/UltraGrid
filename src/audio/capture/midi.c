/**
 * @file   audio/capture/midi.c
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2011-2022 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // defined HAVE_CONFIG_H
#include "config_unix.h"
#include "config_win32.h"

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#else
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#endif // defined HAVE_SDL2

#include <stdio.h>

#include "audio/audio_capture.h"
#include "audio/types.h"
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "song1.h"
#include "types.h"
#include "utils/color_out.h"
#include "utils/ring_buffer.h"

#define DEFAULT_MIDI_BPS 2
#define MIDI_SAMPLE_RATE 48000
#define MOD_NAME "[midi] "

struct state_midi_capture {
        struct audio_frame audio;
        struct ring_buffer *midi_buf;
};

static void audio_cap_midi_done(void *state);

static void audio_cap_midi_probe(struct device_info **available_devices, int *count)
{
        *count = 1;
        *available_devices = calloc(1, sizeof **available_devices);
        strncat((*available_devices)[0].dev, "midi", sizeof (*available_devices)[0].dev - 1);
        strncat((*available_devices)[0].name, "Sample midi song", sizeof (*available_devices)[0].name - 1);
}

static void midi_audio_callback(int chan, void *stream, int len, void *udata)
{
        UNUSED(chan);
        struct state_midi_capture *s = udata;

        ring_buffer_write(s->midi_buf, stream, len);
}

static void * audio_cap_midi_init(const char *cfg)
{
        if (strcmp(cfg, "help") == 0) {
                color_printf("Usage:\n");
                color_printf(TBOLD(TRED("\t-s midi\n")));
                color_printf("\n(currently no user options, default MIDI is played)\n");
                return NULL;
        }

        SDL_Init(SDL_INIT_AUDIO);

        struct state_midi_capture *s = calloc(1, sizeof *s);
        s->audio.bps = audio_capture_bps ? audio_capture_bps : DEFAULT_MIDI_BPS;
        s->audio.ch_count = audio_capture_channels > 0 ? audio_capture_channels : DEFAULT_AUDIO_CAPTURE_CHANNELS;
        s->audio.sample_rate = MIDI_SAMPLE_RATE;

        int audio_format = 0;
        switch (s->audio.bps) {
                case 1: audio_format = AUDIO_S8; break;
                case 2: audio_format = AUDIO_S16LSB; break;
                case 4: audio_format = AUDIO_S32LSB; break;
                default: UG_ASSERT(0 && "BPS can be only 1, 2 or 4");
        }

        if( Mix_OpenAudio(MIDI_SAMPLE_RATE, audio_format,
                                s->audio.ch_count, 4096 ) == -1 ) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error initalizing sound\n");
                goto error;
        }
#ifdef _WIN32
        const char *filename = tmpnam(NULL);
        FILE *f = fopen(filename, "wb");
#else
        char filename[] = P_tmpdir "/uv.midiXXXXXX";
        umask(S_IRWXG|S_IRWXO);
        int fd = mkstemp(filename);
        FILE *f = fd == -1 ? NULL : fdopen(fd, "wb");
#endif
        if (f == NULL) {
                perror("fopen midi");
                goto error;
        }
        size_t nwritten = fwrite(song1, sizeof song1, 1, f);
        fclose(f);
        if (nwritten != 1) {
                unlink(filename);
                goto error;
        }
        Mix_Music *music = Mix_LoadMUS(filename);
        unlink(filename);
        if (music == NULL) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error loading MIDI: %s\n", Mix_GetError());
                goto error;
        }

        s->audio.max_size =
                s->audio.data_len = s->audio.ch_count * s->audio.bps * s->audio.sample_rate /* 1 sec */;
        s->audio.data = malloc(s->audio.data_len);
        s->midi_buf = ring_buffer_init(s->audio.data_len);

        // register grab as a postmix processor
        if (!Mix_RegisterEffect(MIX_CHANNEL_POST, midi_audio_callback, NULL, s)) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Mix_RegisterEffect: %s\n", Mix_GetError());
                goto error;
        }

        if(Mix_PlayMusic(music,-1)==-1){
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error playing MIDI: %s\n", Mix_GetError());
                goto error;
        }
        Mix_Volume(-1, 0);

        log_msg(LOG_LEVEL_NOTICE, MOD_NAME "Initialized MIDI\n");

        return s;
error:
        audio_cap_midi_done(s);
        return NULL;
}

static struct audio_frame *audio_cap_midi_read(void *state)
{
        struct state_midi_capture *s = state;
        s->audio.data_len = ring_buffer_read(s->midi_buf, s->audio.data, s->audio.max_size);
        if (s->audio.data_len == 0) {
                return NULL;
        }
        return &s->audio;
}

static void audio_cap_midi_done(void *state)
{
        Mix_CloseAudio();
        struct state_midi_capture *s = state;
        free(s->audio.data);
        free(s);
}

static void audio_cap_midi_help(const char *state)
{
        UNUSED(state);
}

static const struct audio_capture_info acap_midi_info = {
        audio_cap_midi_probe,
        audio_cap_midi_help,
        audio_cap_midi_init,
        audio_cap_midi_read,
        audio_cap_midi_done
};

REGISTER_MODULE(midi, &acap_midi_info, LIBRARY_CLASS_AUDIO_CAPTURE, AUDIO_CAPTURE_ABI_VERSION);
