/**
 * @file   video_display/decklink.cpp
 * @author Martin Benes     <martinbenesh@gmail.com>
 * @author Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 * @author Petr Holub       <hopet@ics.muni.cz>
 * @author Milos Liska      <xliska@fi.muni.cz>
 * @author Jiri Matela      <matela@ics.muni.cz>
 * @author Dalibor Matura   <255899@mail.muni.cz>
 * @author Martin Pulec     <pulec@cesnet.cz>
 *
 * TestPattern example from BMD SDK was used to consult correct BMD workflow
 * usage and also SignalGenerator was used for scheduled playback.
 */
/*
 * Copyright (c) 2010-2023 CESNET, z. s. p. o.
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
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#define MOD_NAME "[Decklink display] "

#include "audio/types.h"
#include "blackmagic_common.hpp"
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "module.h"
#include "tv.h"
#include "ug_runtime_error.hpp"
#include "utils/macros.h"
#include "utils/math.h"
#include "utils/misc.h"
#include "utils/string.h" // is_prefix_of
#include "video.h"
#include "video_display.h"
#include "video_display/decklink_drift_fix.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "DeckLinkAPIVersion.h"

#ifndef WIN32
#define STDMETHODCALLTYPE
#endif

enum {
        SCHED_RANGE              = 2,
        DEFAULT_MIN_SCHED_FRAMES = 4,
        DEFAULT_MAX_SCHED_FRAMES = DEFAULT_MIN_SCHED_FRAMES + SCHED_RANGE,
        MAX_UNPROC_QUEUE_SIZE    = 10,
};

#define RELEASE_IF_NOT_NULL(x) if ((x) != nullptr) { (x)->Release(); (x) = nullptr; }

static void print_output_modes(IDeckLink *deckLink, const char *query_prop_fcc);
static void display_decklink_done(void *state);
static bool display_decklink_reconfigure(void *state, struct video_desc desc);

// performs command, if failed, displays error and jumps to error label
#define EXIT_IF_FAILED(cmd, name) \
        do {\
                const HRESULT result = cmd;\
                if (FAILED(result)) {;\
                        LOG(LOG_LEVEL_ERROR) << MOD_NAME << name << ": " << bmd_hresult_to_string(result) << "\n";\
                        return FALSE;\
                }\
        } while (0)

// similar as above, but only displays warning
#define CALL_AND_CHECK(cmd, name) \
        do {\
                const HRESULT result = cmd;\
                if (FAILED(result)) {;\
                        LOG(LOG_LEVEL_WARNING) << MOD_NAME << name << ": " << bmd_hresult_to_string(result) << "\n";\
                }\
        } while (0)

using namespace std;

namespace {
class DeckLinkFrame;

struct audio_vals {
        int64_t saved_sync_ts   = INT64_MIN;
        int64_t last_sync_ts    = INT64_MIN;
};

enum audio_sync_val : int64_t {
        deinit = INT64_MIN,
        resync = INT64_MIN + 1,
};

/// Used for scheduled playback only
class PlaybackDelegate : public IDeckLinkVideoOutputCallback // , public IDeckLinkAudioOutputCallback
{
      private:
        chrono::high_resolution_clock::time_point t0 =
            chrono::high_resolution_clock::now();
        uint64_t frames_dropped = 0;
        uint64_t frames_flushed = 0;
        uint64_t frames_late = 0;

        IDeckLinkOutput *m_deckLinkOutput{};
        mutex schedLock;
        queue<DeckLinkFrame *> schedFrames{};
        DeckLinkFrame *lastSchedFrame{};
        long schedSeq{};
        atomic<int64_t> m_audio_sync_ts = audio_sync_val::deinit;
        struct audio_vals m_adata;

      public:
        unsigned m_min_sched_frames = DEFAULT_MIN_SCHED_FRAMES;
        unsigned m_max_sched_frames = DEFAULT_MAX_SCHED_FRAMES;
        BMDTimeValue frameRateDuration{};
        BMDTimeScale frameRateScale{};

        void SetDecklinkOutput(IDeckLinkOutput *ido)
        {
            m_deckLinkOutput = ido;
        }
        void Reset();
        void ResetAudio() { m_audio_sync_ts = audio_sync_val::deinit; }

        virtual ~PlaybackDelegate() {
                Reset();
        };
        void PrintStats();

        // IUnknown needs only a dummy implementation
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/,
                                                 LPVOID * /*ppv*/) override
        {
                return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }

        bool EnqueueFrame(DeckLinkFrame *deckLinkFrame);
        void ScheduleNextFrame();
        void ScheduleAudio(const struct audio_frame *frame, uint32_t *samples);

        HRESULT STDMETHODCALLTYPE
        ScheduledFrameCompleted(IDeckLinkVideoFrame *completedFrame,
                                BMDOutputFrameCompletionResult result) override
        {
                if (result == bmdOutputFrameDisplayedLate){
                        frames_late += 1;
                        LOG(LOG_LEVEL_VERBOSE) << MOD_NAME "Late frame (total: " << frames_late << ")\n";
                } else if (result == bmdOutputFrameDropped){
                        frames_dropped += 1;
                        LOG(LOG_LEVEL_WARNING) << MOD_NAME "Dropped frame (total: " << frames_dropped << ")\n";
                } else if (result == bmdOutputFrameFlushed){
                        frames_flushed += 1;
                        LOG(LOG_LEVEL_WARNING) << MOD_NAME "Flushed frame (total: " << frames_flushed << ")\n";
                }

		if (log_level >= LOG_LEVEL_DEBUG) {
			IDeckLinkTimecode *timecode = NULL;
			if (completedFrame->GetTimecode ((BMDTimecodeFormat) 0, &timecode) == S_OK) {
				BMD_STR timecode_str;
				if (timecode && timecode->GetString(&timecode_str) == S_OK) {
                                        char *timecode_cstr = get_cstr_from_bmd_api_str(timecode_str);
                                        LOG(LOG_LEVEL_DEBUG)
                                            << "Frame " << timecode_cstr
                                            << " output at "
                                            << (double) get_time_in_ns() /
                                                   NS_IN_SEC_DBL
                                            << '\n';
                                        release_bmd_api_str(timecode_str);
                                        free(timecode_cstr);
				}
			}
		}

                ScheduleNextFrame();

		completedFrame->Release();
		return S_OK;
	}

        HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
        {
                return S_OK;
        }
        // virtual HRESULT         RenderAudioSamples (bool preroll);
};

void PlaybackDelegate::PrintStats()
{
        auto now = chrono::high_resolution_clock::now();
        if (chrono::duration_cast<chrono::seconds>(now - t0).count() >= 5) {
                LOG(LOG_LEVEL_VERBOSE)
                    << MOD_NAME << frames_late << " frames late, "
                    << frames_dropped << " dropped, " << frames_flushed
                    << " flushed cumulative\n";
                t0 = now;
        }
}

struct buffer_pool_t {
        queue<DeckLinkFrame *> frame_queue;
        mutex lock;
};

class DeckLinkTimecode : public IDeckLinkTimecode{
                BMDTimecodeBCD timecode;
        public:
                DeckLinkTimecode() : timecode(0) {}
                virtual ~DeckLinkTimecode() = default;
                /* IDeckLinkTimecode */
                virtual BMDTimecodeBCD STDMETHODCALLTYPE GetBCD (void) { return timecode; }
                virtual HRESULT STDMETHODCALLTYPE GetComponents (/* out */ uint8_t *hours, /* out */ uint8_t *minutes, /* out */ uint8_t *seconds, /* out */ uint8_t *frames) { 
                        *frames =   (timecode & 0xf)              + ((timecode & 0xf0) >> 4) * 10;
                        *seconds = ((timecode & 0xf00) >> 8)      + ((timecode & 0xf000) >> 12) * 10;
                        *minutes = ((timecode & 0xf0000) >> 16)   + ((timecode & 0xf00000) >> 20) * 10;
                        *hours =   ((timecode & 0xf000000) >> 24) + ((timecode & 0xf0000000) >> 28) * 10;
                        return S_OK;
                }
                virtual HRESULT STDMETHODCALLTYPE GetString (/* out */ BMD_STR *timecode) {
                        uint8_t hours, minutes, seconds, frames;
                        GetComponents(&hours, &minutes, &seconds, &frames);
                        char timecode_c[16];
                        assert(hours <= 99 && minutes <= 59 && seconds <= 60 && frames <= 99);
                        snprintf(timecode_c, sizeof timecode_c, "%02" PRIu8 ":%02" PRIu8 ":%02" PRIu8 ":%02" PRIu8, hours, minutes, seconds, frames);
                        *timecode = get_bmd_api_str_from_cstr(timecode_c);
                        return *timecode ? S_OK : E_FAIL;
                }
                virtual BMDTimecodeFlags STDMETHODCALLTYPE GetFlags (void)        { return bmdTimecodeFlagDefault; }
                virtual HRESULT STDMETHODCALLTYPE GetTimecodeUserBits (/* out */ BMDTimecodeUserBits *userBits) { if (!userBits) return E_POINTER; else return S_OK; }

                /* IUnknown */
                virtual HRESULT STDMETHODCALLTYPE QueryInterface (REFIID , LPVOID *)        {return E_NOINTERFACE;}
                virtual ULONG STDMETHODCALLTYPE         AddRef ()                                                                       {return 1;}
                virtual ULONG STDMETHODCALLTYPE          Release ()                                                                      {return 1;}
                
                void STDMETHODCALLTYPE SetBCD(BMDTimecodeBCD timecode) { this->timecode = timecode; }
};

struct ChromaticityCoordinates
{
        double RedX;
        double RedY;
        double GreenX;
        double GreenY;
        double BlueX;
        double BlueY;
        double WhiteX;
        double WhiteY;
};

constexpr ChromaticityCoordinates kDefaultRec2020Colorimetrics = { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 };
constexpr double kDefaultMaxDisplayMasteringLuminance        = 1000.0;
constexpr double kDefaultMinDisplayMasteringLuminance        = 0.0001;
constexpr double kDefaultMaxCLL                              = 1000.0;
constexpr double kDefaultMaxFALL                             = 50.0;
enum class HDR_EOTF { NONE = -1, SDR = 0, HDR = 1, PQ = 2, HLG = 3 };

struct HDRMetadata
{
        int64_t                                 EOTF{static_cast<int64_t>(HDR_EOTF::NONE)};
        ChromaticityCoordinates referencePrimaries{kDefaultRec2020Colorimetrics};
        double                                  maxDisplayMasteringLuminance{kDefaultMaxDisplayMasteringLuminance};
        double                                  minDisplayMasteringLuminance{kDefaultMinDisplayMasteringLuminance};
        double                                  maxCLL{kDefaultMaxCLL};
        double                                  maxFALL{kDefaultMaxFALL};

        void                                    Init(const string & fmt);
};

class DeckLinkFrame : public IDeckLinkMutableVideoFrame, public IDeckLinkVideoFrameMetadataExtensions
{
                long width;
                long height;
                long rawBytes;
                BMDPixelFormat pixelFormat;
                unique_ptr<char []> data;

                IDeckLinkTimecode *timecode;

                atomic<ULONG> ref = 1;

                buffer_pool_t &buffer_pool;
                struct HDRMetadata m_metadata;
        protected:
                DeckLinkFrame(long w, long h, long rb, BMDPixelFormat pf, buffer_pool_t & bp, HDRMetadata const & hdr_metadata);

        public:
                virtual ~DeckLinkFrame();
                static DeckLinkFrame *Create(long width, long height, long rawBytes, BMDPixelFormat pixelFormat, buffer_pool_t & buffer_pool, HDRMetadata const & hdr_metadata);

                /* IUnknown */
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
                ULONG STDMETHODCALLTYPE AddRef() override;
                ULONG STDMETHODCALLTYPE Release() override;
                
                /* IDeckLinkVideoFrame */
                long STDMETHODCALLTYPE GetWidth (void) override;
                long STDMETHODCALLTYPE GetHeight (void) override;
                long STDMETHODCALLTYPE GetRowBytes (void) override;
                BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat (void) override;
                BMDFrameFlags STDMETHODCALLTYPE GetFlags (void) override;
                HRESULT STDMETHODCALLTYPE GetBytes (/* out */ void **buffer) override;
                HRESULT STDMETHODCALLTYPE GetTimecode (/* in */ BMDTimecodeFormat format, /* out */ IDeckLinkTimecode **timecode) override;
                HRESULT STDMETHODCALLTYPE GetAncillaryData (/* out */ IDeckLinkVideoFrameAncillary **ancillary) override;

                /* IDeckLinkMutableVideoFrame */
                HRESULT STDMETHODCALLTYPE SetFlags(BMDFrameFlags) override;
                HRESULT STDMETHODCALLTYPE SetTimecode(BMDTimecodeFormat, IDeckLinkTimecode*) override;
                HRESULT STDMETHODCALLTYPE SetTimecodeFromComponents(BMDTimecodeFormat, uint8_t, uint8_t, uint8_t, uint8_t, BMDTimecodeFlags) override;
                HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary*) override;
                HRESULT STDMETHODCALLTYPE SetTimecodeUserBits(BMDTimecodeFormat, BMDTimecodeUserBits) override;

                // IDeckLinkVideoFrameMetadataExtensions interface
                HRESULT STDMETHODCALLTYPE GetInt(BMDDeckLinkFrameMetadataID metadataID, int64_t* value) override;
                HRESULT STDMETHODCALLTYPE GetFloat(BMDDeckLinkFrameMetadataID metadataID, double* value) override;
                HRESULT STDMETHODCALLTYPE GetFlag(BMDDeckLinkFrameMetadataID metadataID, BMD_BOOL* value) override;
                HRESULT STDMETHODCALLTYPE GetString(BMDDeckLinkFrameMetadataID metadataID, BMD_STR * value) override;
                HRESULT STDMETHODCALLTYPE GetBytes(BMDDeckLinkFrameMetadataID metadataID, void* buffer, uint32_t* bufferSize) override;

                int64_t timestamp = INT64_MIN;
};

class DeckLink3DFrame : public DeckLinkFrame, public IDeckLinkVideoFrame3DExtensions
{
        private:
                using DeckLinkFrame::DeckLinkFrame;
                DeckLink3DFrame(long w, long h, long rb, BMDPixelFormat pf, buffer_pool_t & buffer_pool, HDRMetadata const & hdr_metadata);
                unique_ptr<DeckLinkFrame> rightEye; // rightEye ref count is always >= 1 therefore deleted by owner (this class)

        public:
                ~DeckLink3DFrame();
                static DeckLink3DFrame *Create(long width, long height, long rawBytes, BMDPixelFormat pixelFormat, buffer_pool_t & buffer_pool, HDRMetadata const & hdr_metadata);
                
                /* IUnknown */
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
                ULONG STDMETHODCALLTYPE AddRef() override;
                ULONG STDMETHODCALLTYPE Release() override;

                /* IDeckLinkVideoFrame3DExtensions */
                BMDVideo3DPackingFormat STDMETHODCALLTYPE Get3DPackingFormat() override;
                HRESULT STDMETHODCALLTYPE GetFrameForRightEye(IDeckLinkVideoFrame**) override;
};

void PlaybackDelegate::Reset()
{
        RELEASE_IF_NOT_NULL(lastSchedFrame);
        while (!schedFrames.empty()) {
                schedFrames.front()->Release();
                schedFrames.pop();
        }
        schedSeq = 0;
}

bool PlaybackDelegate::EnqueueFrame(DeckLinkFrame *deckLinkFrame)
{
        const unique_lock<mutex> lk(schedLock);
        const unsigned buffered = schedFrames.size();
        if (buffered < MAX_UNPROC_QUEUE_SIZE) {
                schedFrames.push(deckLinkFrame);
                return true;
        }

        deckLinkFrame->Release();
        LOG(LOG_LEVEL_ERROR)
            << MOD_NAME "Queue overflow,  buffered: " << buffered
            << ". This should not happen!\n";
        m_audio_sync_ts = audio_sync_val::resync;
        return false;
}

void PlaybackDelegate::ScheduleNextFrame()
{
        uint32_t i = 0;
        m_deckLinkOutput->GetBufferedVideoFrameCount(&i);
        LOG(LOG_LEVEL_DEBUG) << MOD_NAME << __func__ << " - " << i << " frames buffered\n";

        const unique_lock<mutex> lk(schedLock);
        if (schedFrames.empty()) {
                if (i >= m_min_sched_frames) {
                        return;
                }
                LOG(LOG_LEVEL_WARNING) << MOD_NAME "Missing frame\n";
                m_audio_sync_ts = audio_sync_val::resync;
                m_deckLinkOutput->ScheduleVideoFrame(
                    lastSchedFrame, schedSeq * frameRateDuration,
                    frameRateDuration, frameRateScale);
                schedSeq += 1;
                return;
        }
        while (!schedFrames.empty()) {
                DeckLinkFrame *f = schedFrames.front();
                schedFrames.pop();
                if (++i > m_max_sched_frames) {
                        LOG(LOG_LEVEL_WARNING)
                            << MOD_NAME "Dismissed frame, buffered: " << i - 1
                            << "\n";
                        f->Release();
                        continue;
                }
                RELEASE_IF_NOT_NULL(lastSchedFrame);
                lastSchedFrame = f;
                lastSchedFrame->AddRef();
                if (m_audio_sync_ts <= audio_sync_val::resync &&
                    f->timestamp != INT64_MIN) {
                        m_audio_sync_ts =
                            (uint32_t) (f->timestamp - frameRateDuration * schedSeq *
                                                           90000 / frameRateScale);
                }
                m_deckLinkOutput->ScheduleVideoFrame(
                    f, schedSeq * frameRateDuration, frameRateDuration,
                    frameRateScale);
                schedSeq += 1;
        }
}
} // end of unnamed namespace

#define DECKLINK_MAGIC 0x12de326b

struct state_decklink {
        uint32_t            magic = DECKLINK_MAGIC;
        bool                com_initialized = false;
        PlaybackDelegate            delegate;
        IDeckLink                  *deckLink;
        IDeckLinkOutput            *deckLinkOutput;
        IDeckLinkConfiguration     *deckLinkConfiguration;
        IDeckLinkProfileAttributes *deckLinkAttributes;

        DeckLinkTimecode    *timecode{}; ///< @todo Should be actually allocated dynamically and
                                       ///< its lifespan controlled by AddRef()/Release() methods

        struct video_desc   vid_desc{};
        struct audio_desc   aud_desc {
                2, 48000, 2, AC_PCM
        };

        bool                stereo            = false;
        bool                initialized       = false;
        bool                emit_timecode     = false;
        bool                play_audio        = false; ///< the BMD device will be used also for output audio
        int64_t             max_aud_chans     = BMD_MAX_AUD_CH;

        BMDPixelFormat      pixelFormat{};

        bmd_option          profile_req;
        bmd_option          quad_square_division_split{true, false};
        map<BMDDeckLinkConfigurationID, bmd_option> device_options = {
                { bmdDeckLinkConfigVideoOutputIdleOperation, bmd_option{(int64_t) bmdIdleVideoOutputLastFrame, false} },
                { bmdDeckLinkConfigOutput1080pAsPsF, bmd_option{false, false}},
                { bmdDeckLinkConfigFieldFlickerRemoval, bmd_option{false, false}}, ///< required for interlaced video in low-latency
                { bmdDeckLinkConfigLowLatencyVideoOutput, bmd_option{true, false}}
        };
        HDRMetadata         requested_hdr_mode{};

        buffer_pool_t       buffer_pool;

        bool                low_latency       = true;

        mutex               audio_reconf_lock; ///< for audio and video reconf to be mutually exclusive
        atomic_bool         audio_reconfigure = false;
        bool                keep_device_defaults = false;

        AudioDriftFixer audio_drift_fixer{};
 };

/// @param query_prop_fcc if not NULL, print corresponding BMDDeckLinkAttribute
static void
show_help(bool full, const char *query_prop_fcc = nullptr)
 {
        IDeckLinkIterator*              deckLinkIterator;
        IDeckLink*                      deckLink;
        int                             numDevices = 0;

        col() << "Decklink display options:\n";
        col() << SBOLD(SRED("\t-d decklink")
                       << "[:d[evice]=<device>][:Level{A|B}][:3D][:half-"
                          "duplex][:HDR[=<t>][:drift_fix]]\n");
        col() << SBOLD(SRED("\t-d decklink") << ":[full]help") << " | "
              << SBOLD(SRED("-d decklink") << ":query=<FourCC>"
                                              "\n");
        col() << "\nOptions:\n";
        if (!full) {
                col() << SBOLD("\tfullhelp") << "\tdisplay additional options and more details\n";
        }
        col() << SBOLD("\tdevice") << "\t\tindex or name of output device\n";
        col() << SBOLD("\tLevelA/LevelB") << "\tspecifies 3G-SDI output level\n";
        col() << SBOLD("\t3D") << "\t\t3D stream will be received (see also HDMI3DPacking option)\n";
        col() << SBOLD("\thalf-duplex | full-duplex")
                << "\tset a profile that allows maximal number of simultaneous IOs / set device to better compatibility (3D, dual-link)\n";
        col() << SBOLD("\tHDR[=HDR|PQ|HLG|<int>|help]") << " - enable HDR metadata (optionally specifying EOTF, int 0-7 as per CEA 861.), help for extended help\n";
        col() << SBOLD("\tdrift_fix") << "       activates a time drift fix for the Decklink cards with resampler (experimental)\n";
        if (!full) {
                col() << SBOLD("\tconversion") << "\toutput size conversion, use '-d decklink:fullhelp' for list of conversions\n";
                col() << "\n\t(other options available, use \"" << SBOLD("fullhelp") << "\" to see complete list of options)\n";
        } else {
                col() << SBOLD("\tsingle-link/dual-link/quad-link") << "\tspecifies if the video output will be in a single-link (HD/3G/6G/12G), dual-link HD-SDI mode or quad-link HD/3G/6G/12G\n";
                col() << SBOLD("\ttimecode") << "\temit timecode\n";
                col() << SBOLD("\t[no-]quad-square") << " set Quad-link SDI is output in Square Division Quad Split mode\n";
                col() << SBOLD("\tsynchronized[=m[,M]]")
                      << " use regular scheduled mode for synchrized output"
                         "\n\t\t(m -  minimum "
                         "scheduled frames /default "
                      << DEFAULT_MIN_SCHED_FRAMES
                      << "/, M - max sched\n\t\tframes /default "
                      << DEFAULT_MAX_SCHED_FRAMES << "/), shortcut sync\n";
                col() << SBOLD("\tconversion") << "\toutput size conversion, can be:\n" <<
                                SBOLD("\t\tnone") << " - no conversion\n" <<
                                SBOLD("\t\tltbx") << " - down-converted letterbox SD\n" <<
                                SBOLD("\t\tamph") << " - down-converted anamorphic SD\n" <<
                                SBOLD("\t\t720c") << " - HD720 to HD1080 conversion\n" <<
                                SBOLD("\t\tHWlb") << " - simultaneous output of HD and down-converted letterbox SD\n" <<
                                SBOLD("\t\tHWam") << " - simultaneous output of HD and down-converted anamorphic SD\n" <<
                                SBOLD("\t\tHWcc") << " - simultaneous output of HD and center cut SD\n" <<
                                SBOLD("\t\txcap") << " - simultaneous output of 720p and 1080p cross-conversion\n" <<
                                SBOLD("\t\tua7p") << " - simultaneous output of SD and up-converted anamorphic 720p\n" <<
                                SBOLD("\t\tua1i") << " - simultaneous output of SD and up-converted anamorphic 1080i\n" <<
                                SBOLD("\t\tu47p") << " - simultaneous output of SD and up-converted anamorphic widescreen aspect ratio 14:9 to 720p\n" <<
                                SBOLD("\t\tu41i") << " - simultaneous output of SD and up-converted anamorphic widescreen aspect ratio 14:9 to 1080i\n" <<
                                SBOLD("\t\tup7p") << " - simultaneous output of SD and up-converted pollarbox 720p\n" <<
                                SBOLD("\t\tup1i") << " - simultaneous output of SD and up-converted pollarbox 1080i\n";
                col() << SBOLD("\tHDMI3DPacking") << " can be (used in conjunction with \"3D\" option):\n" <<
				SBOLD("\t\tSideBySideHalf, LineByLine, TopAndBottom, FramePacking, LeftOnly, RightOnly\n");
                col() << SBOLD("\tUse1080PsF[=true|false|keep]") << " flag sets use of PsF on output instead of progressive (default is false)\n";
                col() << SBOLD("\tprofile=<P>") << "\tuse desired device profile:\n";
                print_bmd_device_profiles("\t\t");
                col() << SBOLD("\tmaxresample=<N>") << " maximum amount the resample delta can be when scaling is applied. Measured in Hz\n";
                col() << SBOLD("\tminresample=<N>") << " minimum amount the resample delta can be when scaling is applied. Measured in Hz\n";
                col() << SBOLD("\ttargetbuffer=<N>") << " target amount of samples to have in the buffer (per channel)\n";
                col() << SBOLD("\tkeep-settings") << "\tdo not apply any DeckLink settings by UG than required (keep user-selected defaults)\n";
                col() << SBOLD("\tquery=<FourCC>") << "\tquery specified device argument in help listing\n";
                col() << SBOLD("\t<option_FourCC>=<value>") << "\tarbitrary BMD option (given a FourCC) and corresponding value, i.a.:\n";
                col() << SBOLD("\t\taacl") << "\t\tset maximum audio attenuation on output\n";
        }

        col() << "\nRecognized pixel formats:";
        for_each(uv_to_bmd_codec_map.cbegin(), uv_to_bmd_codec_map.cend(), [](auto const &i) { col() << " " << SBOLD(get_codec_name(i.first)); } );
        cout << "\n";

        col() << "\nDevices:\n";
        // Create an IDeckLinkIterator object to enumerate all DeckLink cards in the system
        bool com_initialized = false;
        deckLinkIterator = create_decklink_iterator(&com_initialized, true);
        if (deckLinkIterator == NULL) {
                return;
        }
        
        // Enumerate all cards in this system
        while (deckLinkIterator->Next(&deckLink) == S_OK)
        {
                string deviceName = bmd_get_device_name(deckLink);
                if (deviceName.empty()) {
                        deviceName = "(unable to get name)";
                }

                // *** Print the model name of the DeckLink card
                col() << "\t" << SBOLD(numDevices) << ") " << SBOLD(deviceName) << "\n";
                if (full) {
                        print_output_modes(deckLink, query_prop_fcc);
                }

                // Increment the total number of DeckLink cards found
                numDevices++;
        
                // Release the IDeckLink instance when we've finished with it to prevent leaks
                deckLink->Release();
        }

        if (!full) {
                col() << "(use \"" << SBOLD("fullhelp") << "\" to see device modes)\n";
        }

        deckLinkIterator->Release();

        decklink_uninitialize(&com_initialized);

        // If no DeckLink cards were found in the system, inform the user
        if (numDevices == 0)
        {
                log_msg(LOG_LEVEL_WARNING, "\nNo Blackmagic Design devices were found.\n");
                return;
        } 

        printf("\n");
        if (full) {
                print_decklink_version();
                printf("\n");
        }
 }

static DeckLinkFrame*
allocate_new_decklink_frame(struct state_decklink *s)
{
        const int linesize =
            vc_get_linesize(s->vid_desc.width, s->vid_desc.color_spec);
        return s->stereo
                   ? DeckLink3DFrame::Create(
                         s->vid_desc.width, s->vid_desc.height, linesize,
                         s->pixelFormat, s->buffer_pool, s->requested_hdr_mode)
                   : DeckLinkFrame::Create(
                         s->vid_desc.width, s->vid_desc.height, linesize,
                         s->pixelFormat, s->buffer_pool, s->requested_hdr_mode);
}

static struct video_frame *
display_decklink_getf(void *state)
{
        struct state_decklink *s = (struct state_decklink *)state;
        assert(s->magic == DECKLINK_MAGIC);

        if (!s->initialized) {
                return nullptr;
        }

        if (s->audio_reconfigure) {
                if (display_decklink_reconfigure(s, s->vid_desc) != TRUE) {
                        return nullptr;
                }
                s->audio_reconfigure = false;
        }

        struct video_frame *out = vf_alloc_desc(s->vid_desc);
        static auto dispose = [](struct video_frame *frame) {
                vf_free(frame);
        };
        out->callbacks.dispose = dispose;

        const int linesize = vc_get_linesize(s->vid_desc.width, s->vid_desc.color_spec);
        DeckLinkFrame *deckLinkFrame = nullptr;
        lock_guard<mutex> lg(s->buffer_pool.lock);

        while (!s->buffer_pool.frame_queue.empty()) {
                auto tmp = s->buffer_pool.frame_queue.front();
                DeckLinkFrame *frame =
                    s->stereo ? dynamic_cast<DeckLink3DFrame *>(tmp)
                              : dynamic_cast<DeckLinkFrame *>(tmp);
                s->buffer_pool.frame_queue.pop();
                if (!frame || // wrong type
                    frame->GetWidth() != (long)s->vid_desc.width ||
                    frame->GetHeight() != (long)s->vid_desc.height ||
                    frame->GetRowBytes() != linesize || frame->GetPixelFormat() != s->pixelFormat) {
                        tmp->Release();
                } else {
                        deckLinkFrame = frame;
                        deckLinkFrame->AddRef();
                        break;
                }
        }
        if (!deckLinkFrame) {
                deckLinkFrame = allocate_new_decklink_frame(s);
        }
        out->callbacks.dispose_udata = (void *) deckLinkFrame;

        deckLinkFrame->GetBytes((void **)&out->tiles[0].data);

        if (s->stereo) {
                IDeckLinkVideoFrame *deckLinkFrameRight = nullptr;
                DeckLink3DFrame *frame3D = dynamic_cast<DeckLink3DFrame *>(deckLinkFrame);
                assert(frame3D != nullptr);
                frame3D->GetFrameForRightEye(&deckLinkFrameRight);
                deckLinkFrameRight->GetBytes((void **)&out->tiles[1].data);
                // release immedieatelly (parent still holds the reference)
                deckLinkFrameRight->Release();
        }

        return out;
}

static void update_timecode(DeckLinkTimecode *tc, double fps)
{
        const float epsilon = 0.005;
        uint8_t hours, minutes, seconds, frames;
        BMDTimecodeBCD bcd;
        bool dropFrame = false;

        if(ceil(fps) - fps > epsilon) { /* NTSCi drop framecode  */
                dropFrame = true;
        }

        tc->GetComponents (&hours, &minutes, &seconds, &frames);
        frames++;

        if((double) frames > fps - epsilon) {
                frames = 0;
                seconds++;
                if(seconds >= 60) {
                        seconds = 0;
                        minutes++;
                        if(dropFrame) {
                                if(minutes % 10 != 0)
                                        seconds = 2;
                        }
                        if(minutes >= 60) {
                                minutes = 0;
                                hours++;
                                if(hours >= 24) {
                                        hours = 0;
                                }
                        }
                }
        }

        bcd = (frames % 10) | (frames / 10) << 4 | (seconds % 10) << 8 | (seconds / 10) << 12 | (minutes % 10)  << 16 | (minutes / 10) << 20 |
                (hours % 10) << 24 | (hours / 10) << 28;

        tc->SetBCD(bcd);
}

static bool display_decklink_putf(void *state, struct video_frame *frame,
                                 [[maybe_unused]] long long timeout_ns)
{
        struct state_decklink *s = (struct state_decklink *)state;
        bool ret = true;

        if (frame == NULL)
                return true;

        assert(s->magic == DECKLINK_MAGIC);

        if (frame->color_spec == R10k && get_commandline_param(R10K_FULL_OPT) == nullptr) {
                for (unsigned i = 0; i < frame->tile_count; ++i) {
                        r10k_full_to_limited(frame->tiles[i].data, frame->tiles[i].data, frame->tiles[i].data_len);
                }
        }

        auto *deckLinkFrame = (DeckLinkFrame *) frame->callbacks.dispose_udata;
        if (s->emit_timecode) {
                deckLinkFrame->SetTimecode(bmdTimecodeRP188Any, s->timecode);
        }

        if (s->low_latency) {
                s->deckLinkOutput->DisplayVideoFrameSync(deckLinkFrame);
                deckLinkFrame->Release();
        } else {
                deckLinkFrame->timestamp = frame->timestamp;
                ret = s->delegate.EnqueueFrame(deckLinkFrame);
        }
        if(s->emit_timecode) {
                update_timecode(s->timecode, s->vid_desc.fps);
        }

        frame->callbacks.dispose(frame);

        s->delegate.PrintStats();

        return ret;
}

static BMDDisplayMode get_mode(IDeckLinkOutput *deckLinkOutput, struct video_desc desc, BMDTimeValue *frameRateDuration,
		BMDTimeScale        *frameRateScale, bool stereo)
{	IDeckLinkDisplayModeIterator     *displayModeIterator;
        IDeckLinkDisplayMode*             deckLinkDisplayMode;
        BMDDisplayMode			  displayMode = bmdModeUnknown;
        
        // Populate the display mode combo with a list of display modes supported by the installed DeckLink card
        if (FAILED(deckLinkOutput->GetDisplayModeIterator(&displayModeIterator)))
        {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Fatal: cannot create display mode iterator.\n");
                return bmdModeUnknown;
        }

        while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK)
        {
                BMD_STR modeNameString;
                if (deckLinkDisplayMode->GetName(&modeNameString) == S_OK)
                {
                        char *modeNameCString = get_cstr_from_bmd_api_str(modeNameString);
                        if (deckLinkDisplayMode->GetWidth() == (long) desc.width &&
                                        deckLinkDisplayMode->GetHeight() == (long) desc.height)
                        {
                                double displayFPS;
                                BMDFieldDominance dominance;
                                bool interlaced;

                                dominance = deckLinkDisplayMode->GetFieldDominance();
                                if (dominance == bmdLowerFieldFirst ||
                                                dominance == bmdUpperFieldFirst) {
					if (dominance == bmdLowerFieldFirst) {
						log_msg(LOG_LEVEL_WARNING, MOD_NAME "Lower field first format detected, fields can be switched! If so, please report a bug to " PACKAGE_BUGREPORT "\n");
					}
                                        interlaced = true;
                                } else { // progressive, psf, unknown
                                        interlaced = false;
                                }

                                deckLinkDisplayMode->GetFrameRate(frameRateDuration,
                                                frameRateScale);
                                displayFPS = (double) *frameRateScale / *frameRateDuration;
                                if (fabs(desc.fps - displayFPS) < 0.01 && (desc.interlacing == INTERLACED_MERGED) == interlaced) {
                                        log_msg(LOG_LEVEL_INFO, MOD_NAME "Selected mode: %s%s\n", modeNameCString,
                                                        stereo ? " (3D)" : "");
                                        displayMode = deckLinkDisplayMode->GetDisplayMode();
                                        release_bmd_api_str(modeNameString);
                                        free(modeNameCString);
                                        deckLinkDisplayMode->Release();
                                        break;
                                }
                        }
                        release_bmd_api_str(modeNameString);
                        free((void *) modeNameCString);
                }
                deckLinkDisplayMode->Release();
        }
        displayModeIterator->Release();
        
        return displayMode;
}

static int enable_audio(struct state_decklink *s, int bps, int channels)
{
        const BMDAudioSampleType sample_type =
            bps == 2 ? bmdAudioSampleType16bitInteger
                     : bmdAudioSampleType32bitInteger;
        const BMDAudioOutputStreamType stream_type =
            s->low_latency ? bmdAudioOutputStreamContinuous
                           : bmdAudioOutputStreamTimestamped;
        EXIT_IF_FAILED(
            s->deckLinkOutput->EnableAudioOutput(
                bmdAudioSampleRate48kHz, sample_type, channels, stream_type),
            "EnableAudioOutput");
        return TRUE;
}

static bool
display_decklink_reconfigure(void *state, struct video_desc desc)
{
        auto *s = (struct state_decklink *) state;
        assert(s->magic == DECKLINK_MAGIC);

        BMDDisplayMode                    displayMode;
        BMD_BOOL                          supported;
        HRESULT                           result;

        const unique_lock<mutex> lk(s->audio_reconf_lock);
        s->delegate.ResetAudio(); // disables audio until full reconf

        s->vid_desc = desc;

        if (s->initialized) {
                if (!s->low_latency) {
                        CALL_AND_CHECK(s->deckLinkOutput->StopScheduledPlayback(
                                           0, nullptr, 0),
                                       "StopScheduledPlayback");
                }
                s->deckLinkOutput->SetScheduledFrameCompletionCallback(nullptr);
                CALL_AND_CHECK(s->deckLinkOutput->DisableVideoOutput(),
                               "DisableVideoOutput");
                if (s->play_audio) {
                        CALL_AND_CHECK(s->deckLinkOutput->DisableAudioOutput(),
                                       "DisableAudioOutput");
                }
                s->initialized = false;
        }

        s->delegate.Reset();

        if (desc.color_spec == R10k && get_commandline_param(R10K_FULL_OPT) == nullptr) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Using limited range R10k as specified by BMD, use '--param "
                                R10K_FULL_OPT "' to override.\n");
        }

        auto it = std::find_if(uv_to_bmd_codec_map.begin(),
                        uv_to_bmd_codec_map.end(),
                        [&desc](const std::pair<codec_t, BMDPixelFormat>& el){ return el.first == desc.color_spec; });
        if (it == uv_to_bmd_codec_map.end()) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unsupported pixel format!\n");
                return false;
        }
        s->pixelFormat = it->second;

        if (desc.tile_count <= 2 && desc.tile_count != (s->stereo ? 2 : 1)) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Stereo %s enabled but receiving %u streams. %sabling "
                                "it. This behavior is experimental so please report any problems. "
                                "You can also specify (or not) `3D` option explicitly.\n"
                                , s->stereo ? "" : "not", desc.tile_count, s->stereo ? "dis" : "en");
                s->stereo = !s->stereo;
        }

        if (s->stereo) {
                bmd_check_stereo_profile(s->deckLink);
                if ((int) desc.tile_count != 2) {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "In stereo mode exactly "
                                        "2 streams expected, %d received.\n", desc.tile_count);
                        return false;
                }
        } else {
                if ((int) desc.tile_count == 2) {
                        log_msg(LOG_LEVEL_WARNING, MOD_NAME "Received 2 streams but stereo mode is not enabled! Did you forget a \"3D\" parameter?\n");
                }
        }

        BMDVideoOutputFlags outputFlags = bmdVideoOutputFlagDefault;
        BMDSupportedVideoModeFlags supportedFlags = bmdSupportedVideoModeDefault;

        displayMode =
            get_mode(s->deckLinkOutput, desc, &s->delegate.frameRateDuration,
                     &s->delegate.frameRateScale, s->stereo);
        if (displayMode == bmdModeUnknown) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Could not find suitable video mode.\n");
                return false;
        }

        if (s->emit_timecode) {
                outputFlags = (BMDVideoOutputFlags)(outputFlags | bmdVideoOutputRP188);
        }

        if (s->stereo) {
                outputFlags = (BMDVideoOutputFlags)(outputFlags | bmdVideoOutputDualStream3D);
                supportedFlags = (BMDSupportedVideoModeFlags)(supportedFlags |
                                                              bmdSupportedVideoModeDualStream3D);
        }

        const bmd_option subsampling_444(codec_is_a_rgb(desc.color_spec),
                                         false); // we don't have pixfmt for 444 YCbCr
        subsampling_444.device_write(s->deckLinkConfiguration, bmdDeckLinkConfig444SDIVideoOutput,
                                     MOD_NAME);

        if (!s->keep_device_defaults &&
            s->device_options.find(bmdDeckLinkConfigSDIOutputLinkConfiguration) ==
                s->device_options.end()) {
                const int64_t link = desc.width == 7680 ? bmdLinkConfigurationQuadLink
                                                        : bmdLinkConfigurationSingleLink;
                bmd_option(link).device_write(s->deckLinkConfiguration,
                                              bmdDeckLinkConfigSDIOutputLinkConfiguration,
                                              MOD_NAME);
        }

        int64_t link = 0;
        s->deckLinkConfiguration->GetInt(bmdDeckLinkConfigSDIOutputLinkConfiguration, &link);
        if (!s->keep_device_defaults && s->profile_req.is_default() &&
            link == bmdLinkConfigurationQuadLink) {
                LOG(LOG_LEVEL_WARNING)
                    << MOD_NAME "Quad-link detected - setting 1-subdevice-1/2-duplex "
                                "profile automatically, use 'profile=keep' to override.\n";
                decklink_set_profile(
                    s->deckLink, bmd_option((int64_t)bmdProfileOneSubDeviceHalfDuplex), s->stereo);
        } else if (link == bmdLinkConfigurationQuadLink &&
                   (!s->profile_req.keep() &&
                    s->profile_req.get_int() != bmdProfileOneSubDeviceHalfDuplex)) {
                LOG(LOG_LEVEL_WARNING) << MOD_NAME "Setting quad-link and an incompatible device "
                                                   "profile may not be supported!\n";
        }

        BMD_BOOL quad_link_supp = BMD_FALSE;
        if (s->deckLinkAttributes->GetFlag(BMDDeckLinkSupportsQuadLinkSDI, &quad_link_supp) ==
                S_OK &&
            quad_link_supp == BMD_TRUE) {
                s->quad_square_division_split.device_write(
                    s->deckLinkConfiguration,
                    bmdDeckLinkConfigQuadLinkSDIVideoOutputSquareDivisionSplit, MOD_NAME);
        }

        const BMDVideoOutputConversionMode conversion_mode =
            s->device_options.find(bmdDeckLinkConfigVideoOutputConversionMode) !=
                    s->device_options.end()
                ? (BMDVideoOutputConversionMode)s->device_options
                      .at(bmdDeckLinkConfigVideoOutputConversionMode)
                      .get_int()
                : (BMDVideoOutputConversionMode)bmdNoVideoOutputConversion;
        EXIT_IF_FAILED(s->deckLinkOutput->DoesSupportVideoMode(
                           bmdVideoConnectionUnspecified, displayMode, s->pixelFormat,
                           conversion_mode, supportedFlags, nullptr, &supported),
                       "DoesSupportVideoMode");
        if (!supported) {
                log_msg(LOG_LEVEL_ERROR,
                        MOD_NAME "Requested parameters "
                                 "combination not supported - %d * %dx%d@%f, timecode %s.\n",
                        desc.tile_count, desc.width, desc.height, desc.fps,
                        (outputFlags & bmdVideoOutputRP188 ? "ON" : "OFF"));
                return false;
        }

        result = s->deckLinkOutput->EnableVideoOutput(displayMode, outputFlags);
        if (FAILED(result)) {
                if (result == E_ACCESSDENIED) {
                        log_msg(LOG_LEVEL_ERROR,
                                MOD_NAME "Unable to access the hardware or output "
                                         "stream currently active (another application "
                                         "using it?).\n");
                } else {
                        LOG(LOG_LEVEL_ERROR)
                            << MOD_NAME << "EnableVideoOutput: " << bmd_hresult_to_string(result)
                            << "\n";
                }
                return false;
        }

        if (s->play_audio) {
                if (!enable_audio(s, s->aud_desc.bps, s->aud_desc.ch_count)) {
                        return false;
                }
        }

        if (!s->low_latency) {
                // Provide this class as a delegate to a video output interface
                s->deckLinkOutput->SetScheduledFrameCompletionCallback(
                    &s->delegate);
                auto *f = allocate_new_decklink_frame(s);
                for (unsigned i = 0; i < (s->delegate.m_min_sched_frames +
                                          s->delegate.m_min_sched_frames) /
                                             2;
                     ++i) {
                        f->AddRef();
                        const bool ret = s->delegate.EnqueueFrame(f);
                        assert(ret);
                        s->delegate.ScheduleNextFrame();
                }
                f->Release(); // release initial reference from alloc
                result = s->deckLinkOutput->StartScheduledPlayback(
                    0, s->delegate.frameRateScale, 1.0);
                if (FAILED(result)) {
                        LOG(LOG_LEVEL_ERROR)
                            << MOD_NAME << "StartScheduledPlayback (video): "
                            << bmd_hresult_to_string(result) << "\n";
                        s->deckLinkOutput->DisableVideoOutput();
                        return false;
                }
        }

        s->initialized = true;
        s->audio_reconfigure = false;
        return true;
}

static void display_decklink_probe(struct device_info **available_cards, int *count, void (**deleter)(void *))
{
        UNUSED(deleter);
        IDeckLinkIterator*              deckLinkIterator;
        IDeckLink*                      deckLink;

        *count = 0;
        *available_cards = nullptr;

        bool com_initialized = false;
        deckLinkIterator = create_decklink_iterator(&com_initialized, false);
        if (deckLinkIterator == NULL) {
                return;
        }

        // Enumerate all cards in this system
        while (deckLinkIterator->Next(&deckLink) == S_OK)
        {
                string deviceName = bmd_get_device_name(deckLink);
		if (deviceName.empty()) {
			deviceName = "(unknown)";
		}

                *count += 1;
                *available_cards = (struct device_info *)
                        realloc(*available_cards, *count * sizeof(struct device_info));
                memset(*available_cards + *count - 1, 0, sizeof(struct device_info));
                snprintf((*available_cards)[*count - 1].dev, sizeof (*available_cards)[*count - 1].dev, ":device=%d", *count - 1);
                snprintf((*available_cards)[*count - 1].extra, sizeof (*available_cards)[*count - 1].extra, "\"embeddedAudioAvailable\":\"t\"");
                (*available_cards)[*count - 1].repeatable = false;

		strncpy((*available_cards)[*count - 1].name, deviceName.c_str(),
				sizeof (*available_cards)[*count - 1].name - 1);

                dev_add_option(&(*available_cards)[*count - 1], "3D", "3D", "3D", ":3D", true);
                dev_add_option(&(*available_cards)[*count - 1], "Profile", "Duplex profile can be one of: 1dhd, 2dhd, 2dfd, 4dhd, keep", "profile", ":profile=", false);

                // Release the IDeckLink instance when we've finished with it to prevent leaks
                deckLink->Release();
        }

        deckLinkIterator->Release();
        decklink_uninitialize(&com_initialized);
}

static bool settings_init(struct state_decklink *s, const char *fmt,
                string &cardId) {
        if (strlen(fmt) == 0) {
                return true;
        }

        auto tmp = static_cast<char *>(alloca(strlen(fmt) + 1));
        strcpy(tmp, fmt);
        char *ptr;
        char *save_ptr = nullptr;

        ptr = strtok_r(tmp, ":", &save_ptr);
        assert(ptr != nullptr);
        int i = 0;
        bool first_option_is_device = true;
        while (ptr[i] != '\0') {
                if (!isdigit(ptr[i]) && ptr[i] != ',') {
                        first_option_is_device = false;
                        break;
                }
                i++;
        }
        if (first_option_is_device) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Unnamed device index "
                                "deprecated. Use \"device=%s\" instead.\n", ptr);
                cardId = ptr;
                ptr = strtok_r(nullptr, ":", &save_ptr);
        }

        while (ptr != nullptr)  {
                const char *val = strchr(ptr, '=') + 1;
                if (IS_KEY_PREFIX(ptr, "device")) {
                        cardId = strchr(ptr, '=') + 1;
                } else if (strcasecmp(ptr, "3D") == 0) {
                        s->stereo = true;
                } else if (strcasecmp(ptr, "timecode") == 0) {
                        s->emit_timecode = true;
                } else if (strcasecmp(ptr, "single-link") == 0) {
                        s->device_options[bmdDeckLinkConfigSDIOutputLinkConfiguration].set_int(bmdLinkConfigurationSingleLink);
                } else if (strcasecmp(ptr, "dual-link") == 0) {
                        s->device_options[bmdDeckLinkConfigSDIOutputLinkConfiguration].set_int(bmdLinkConfigurationDualLink);
                } else if (strcasecmp(ptr, "quad-link") == 0) {
                        s->device_options[bmdDeckLinkConfigSDIOutputLinkConfiguration].set_int(bmdLinkConfigurationQuadLink);
                } else if (strstr(ptr, "profile=") == ptr) {
                        s->profile_req.parse(ptr);
                } else if (strcasecmp(ptr, "full-duplex") == 0) {
                        s->profile_req.set_int(bmdProfileOneSubDeviceFullDuplex);
                } else if (strcasecmp(ptr, "half-duplex") == 0) {
                        s->profile_req.set_int(bmdDuplexHalf);
                } else if (strcasecmp(ptr, "LevelA") == 0) {
                        s->device_options[bmdDeckLinkConfigSMPTELevelAOutput].set_flag(true);
                } else if (strcasecmp(ptr, "LevelB") == 0) {
                        s->device_options[bmdDeckLinkConfigSMPTELevelAOutput].set_flag(false);
                } else if (strncasecmp(ptr, "HDMI3DPacking=", strlen("HDMI3DPacking=")) == 0) {
                        char *packing = ptr + strlen("HDMI3DPacking=");
                        if (strcasecmp(packing, "SideBySideHalf") == 0) {
                                s->device_options[bmdDeckLinkConfigHDMI3DPackingFormat].set_int(bmdVideo3DPackingSidebySideHalf);
                        } else if (strcasecmp(packing, "LineByLine") == 0) {
                                s->device_options[bmdDeckLinkConfigHDMI3DPackingFormat].set_int(bmdVideo3DPackingLinebyLine);
                        } else if (strcasecmp(packing, "TopAndBottom") == 0) {
                                s->device_options[bmdDeckLinkConfigHDMI3DPackingFormat].set_int(bmdVideo3DPackingTopAndBottom);
                        } else if (strcasecmp(packing, "FramePacking") == 0) {
                                s->device_options[bmdDeckLinkConfigHDMI3DPackingFormat].set_int(bmdVideo3DPackingFramePacking);
                        } else if (strcasecmp(packing, "LeftOnly") == 0) {
                                s->device_options[bmdDeckLinkConfigHDMI3DPackingFormat].set_int(bmdVideo3DPackingRightOnly);
                        } else if (strcasecmp(packing, "RightOnly") == 0) {
                                s->device_options[bmdDeckLinkConfigHDMI3DPackingFormat].set_int(bmdVideo3DPackingLeftOnly);
                        } else {
                                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unknown HDMI 3D packing %s.\n", packing);
                                return false;
                        }
                } else if (strncasecmp(ptr, "audio_level=", strlen("audio_level=")) == 0) {
                        s->device_options
                            [bmdDeckLinkConfigAnalogAudioConsumerLevels] =
                            bmd_option(bmd_parse_audio_levels(val));
                } else if (IS_KEY_PREFIX(ptr, "conversion")) {
                        s->device_options[bmdDeckLinkConfigVideoOutputConversionMode].parse(strchr(ptr, '=') + 1);
                } else if (is_prefix_of(ptr, "Use1080pNotPsF") || is_prefix_of(ptr, "Use1080PsF")) {
                        s->device_options[bmdDeckLinkConfigOutput1080pAsPsF].parse(strchr(ptr, '=') + 1);
                        if (strncasecmp(ptr, "Use1080pNotPsF", strlen("Use1080pNotPsF")) == 0) { // compat, inverse
                                s->device_options[bmdDeckLinkConfigOutput1080pAsPsF].set_flag(s->device_options[bmdDeckLinkConfigOutput1080pAsPsF].get_flag());
                        }
                } else if (strcasecmp(ptr, "low-latency") == 0 || strcasecmp(ptr, "no-low-latency") == 0) {
                        LOG(LOG_LEVEL_WARNING)
                            << MOD_NAME
                            << "Deprecated, do not use - "
                               "see option \"synchroninzed\" instead.\n";
                        s->low_latency = strcasecmp(ptr, "low-latency") == 0;
                } else if (IS_PREFIX(ptr, "synchronized")) {
                        s->low_latency = false;
                        ptr = strchr(ptr, '=');
                        if (ptr != nullptr) {
                                ptr += 1;
                                s->delegate.m_max_sched_frames =
                                    SCHED_RANGE +
                                    (s->delegate.m_min_sched_frames =
                                         stoi(ptr));
                                if (strchr(ptr, ',') != nullptr) {
                                        s->delegate.m_max_sched_frames =
                                            stoi(strchr(ptr, ',') + 1);
                                }
                        }
                } else if (strcasecmp(ptr, "quad-square") == 0 || strcasecmp(ptr, "no-quad-square") == 0) {
                        s->quad_square_division_split.set_flag(strcasecmp(ptr, "quad-square") == 0);
                } else if (strncasecmp(ptr, "hdr", strlen("hdr")) == 0) {
                        s->requested_hdr_mode.EOTF = static_cast<int64_t>(HDR_EOTF::HDR); // default
                        if (strncasecmp(ptr, "hdr=", strlen("hdr=")) == 0) {
                                try {
                                        s->requested_hdr_mode.Init(ptr + strlen("hdr="));
                                } catch (ug_no_error const &e) {
                                        return false;
                                } catch (exception const &e) {
                                        LOG(LOG_LEVEL_ERROR) << MOD_NAME << "HDR mode init: " << e.what() << "\n";
                                        return false;
                                }
                        }
                } else if (strstr(ptr, "keep-settings") == ptr) {
                        s->keep_device_defaults = true;
                } else if (strstr(ptr, "drift_fix") == ptr) {
                        s->audio_drift_fixer.enable();
                } else if (strncasecmp(ptr, "maxresample=", strlen("maxresample=")) == 0) {
                        s->audio_drift_fixer.set_max_hz(parse_uint32(strchr(ptr, '=') + 1));
                } else if (strncasecmp(ptr, "minresample=", strlen("minresample=")) == 0) {
                        s->audio_drift_fixer.set_min_hz(parse_uint32(strchr(ptr, '=') + 1));
                } else if (strncasecmp(ptr, "targetbuffer=", strlen("targetbuffer=")) == 0) {
                        s->audio_drift_fixer.set_target_buffer(parse_uint32(strchr(ptr, '=') + 1));
                } else if ((strchr(ptr, '=') != nullptr && strchr(ptr, '=') - ptr == 4) || strlen(ptr) == 4) {
                        s->device_options[(BMDDeckLinkConfigurationID) bmd_read_fourcc(ptr)].parse(strchr(ptr, '=') + 1);
                } else {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "unknown option in config string: %s\n", ptr);
                        return false;
                }
                ptr = strtok_r(nullptr, ":", &save_ptr);
        }

        return true;
}

static void *display_decklink_init(struct module *parent, const char *fmt, unsigned int flags)
{
        IDeckLinkIterator*                              deckLinkIterator;
        HRESULT                                         result;
        string                                          cardId("0");
        int                                             dnum = 0;
        IDeckLinkConfiguration*         deckLinkConfiguration = NULL;
        // for Decklink Studio which has switchable XLR - analog 3 and 4 or AES/EBU 3,4 and 5,6
        BMDAudioOutputAnalogAESSwitch audioConnection = (BMDAudioOutputAnalogAESSwitch) 0;

        if (strcmp(fmt, "help") == 0 || strcmp(fmt, "fullhelp") == 0) {
                show_help(strcmp(fmt, "fullhelp") == 0);
                return INIT_NOERR;
        }
        if (IS_KEY_PREFIX(fmt, "query")) {
                show_help(true, strchr(fmt, '=') + 1);
                return INIT_NOERR;
        }

        if (!blackmagic_api_version_check()) {
                return NULL;
        }

        auto *s = new state_decklink();
        s->audio_drift_fixer.set_root(get_root_module(parent));

        bool succeeded = false;
        try {
                succeeded = settings_init(s, fmt, cardId);
        } catch (exception &e) {
                if (strcmp(e.what(), "stoi") == 0 ||
                    strcmp(e.what(), "stod") == 0) {
                        MSG(ERROR, "Invalid number passed where numeric "
                                   "argument expected!\n");
                } else {
                        MSG(ERROR, "%s\n", e.what());
                }
        }
        if (!succeeded) {
                delete s;
                return NULL;
        }

        // Initialize the DeckLink API
        deckLinkIterator = create_decklink_iterator(&s->com_initialized, true);
        if (!deckLinkIterator) {
                display_decklink_done(s);
                return nullptr;
        }

        // Connect to the first DeckLink instance
        IDeckLink    *deckLink;
        while (deckLinkIterator->Next(&deckLink) == S_OK)
        {
                string deviceName = bmd_get_device_name(deckLink);

                if (!deviceName.empty() && deviceName == cardId) {
                        s->deckLink = deckLink;
                        break;
                }
                if (isdigit(cardId.c_str()[0]) && dnum == atoi(cardId.c_str())) {
                        s->deckLink = deckLink;
                        break;
                }

                deckLink->Release();
                dnum++;
        }
        deckLinkIterator->Release();
        if (s->deckLink == nullptr) {
                LOG(LOG_LEVEL_ERROR) << "No DeckLink PCI card " << cardId << " found\n";
                display_decklink_done(s);
                return nullptr;
        }
        // Print the model name of the DeckLink card
        string deviceName = bmd_get_device_name(s->deckLink);
        if (!deviceName.empty()) {
                LOG(LOG_LEVEL_INFO) << MOD_NAME "Using device " << deviceName << "\n";
        }

        // Get IDeckLinkAttributes object
        result = s->deckLink->QueryInterface(
            IID_IDeckLinkProfileAttributes,
            reinterpret_cast<void **>(&s->deckLinkAttributes));
        if (result != S_OK) {
                LOG(LOG_LEVEL_WARNING)
                    << MOD_NAME "Could not query device attributes: "
                    << bmd_hresult_to_string(result) << "\n";
                display_decklink_done(s);
                return nullptr;
        }

        if(flags & (DISPLAY_FLAG_AUDIO_EMBEDDED | DISPLAY_FLAG_AUDIO_AESEBU | DISPLAY_FLAG_AUDIO_ANALOG)) {
                s->play_audio = true;
                switch(flags & (DISPLAY_FLAG_AUDIO_EMBEDDED | DISPLAY_FLAG_AUDIO_AESEBU | DISPLAY_FLAG_AUDIO_ANALOG)) {
                        case DISPLAY_FLAG_AUDIO_EMBEDDED:
                                audioConnection = (BMDAudioOutputAnalogAESSwitch) 0;
                                break;
                        case DISPLAY_FLAG_AUDIO_AESEBU:
                                audioConnection = bmdAudioOutputSwitchAESEBU;
                                break;
                        case DISPLAY_FLAG_AUDIO_ANALOG:
                                audioConnection = bmdAudioOutputSwitchAnalog;
                                break;
                        default:
                                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Unsupporetd audio connection.\n");
                                abort();
                }
                if (s->deckLinkAttributes->GetInt(
                        audioConnection == 0
                            ? BMDDeckLinkMaximumAudioChannels
                            : BMDDeckLinkMaximumAnalogAudioOutputChannels,
                        &s->max_aud_chans) != S_OK) {
                                LOG(LOG_LEVEL_WARNING) << "Cannot get maximum auudio channels!\n";
                }
        } else {
                s->play_audio = false;
        }

        if(s->emit_timecode) {
                s->timecode = new DeckLinkTimecode;
        } else {
                s->timecode = NULL;
        }

        if (!s->keep_device_defaults && !s->profile_req.keep()) {
                decklink_set_profile(s->deckLink, s->profile_req, s->stereo);
        }

        // Obtain the audio/video output interface (IDeckLinkOutput)
        if ((result = s->deckLink->QueryInterface(
                 IID_IDeckLinkOutput, reinterpret_cast<void **>(&s->deckLinkOutput))) != S_OK) {
                log_msg(LOG_LEVEL_ERROR,
                        MOD_NAME "Could not obtain the IDeckLinkOutput interface: %08x\n",
                        (int)result);
                display_decklink_done(s);
                return nullptr;
        }

        // Query the DeckLink for its configuration interface
        result = s->deckLink->QueryInterface(IID_IDeckLinkConfiguration,
                                             reinterpret_cast<void **>(&deckLinkConfiguration));
        s->deckLinkConfiguration = deckLinkConfiguration;
        if (result != S_OK) {
                log_msg(LOG_LEVEL_ERROR,
                        "Could not obtain the IDeckLinkConfiguration interface: %08x\n",
                        (int)result);
                display_decklink_done(s);
                return nullptr;
         }

        for (const auto &o : s->device_options) {
                if (s->keep_device_defaults && !o.second.is_user_set()) {
                        continue;
                }
                if (!o.second.device_write(deckLinkConfiguration, o.first, MOD_NAME)) {
                        display_decklink_done(s);
                        return nullptr;
                }
        }

        if (s->requested_hdr_mode.EOTF != static_cast<int64_t>(HDR_EOTF::NONE)) {
                BMD_BOOL hdr_supp = BMD_FALSE;
                if (s->deckLinkAttributes->GetFlag(BMDDeckLinkSupportsHDRMetadata, &hdr_supp) !=
                        S_OK) {
                                LOG(LOG_LEVEL_WARNING)
                                    << MOD_NAME
                                    << "HDR requested, but unable to validate HDR support. Will "
                                       "try to pass it anyway which may result in blank image if "
                                       "not supported - remove the option if so.\n";
                } else {
                        if (hdr_supp != BMD_TRUE) {
                                LOG(LOG_LEVEL_ERROR)
                                    << MOD_NAME
                                    << "HDR requested, but card doesn't support that.\n";
                                display_decklink_done(s);
                                return nullptr;
                        }
                }

                BMD_BOOL rec2020_supp = BMD_FALSE;
                if (s->deckLinkAttributes->GetFlag(BMDDeckLinkSupportsColorspaceMetadata,
                                                   &rec2020_supp) != S_OK) {
                        LOG(LOG_LEVEL_WARNING)
                            << MOD_NAME << "Cannot check Rec. 2020 color space metadata support.\n";
                } else {
                        if (rec2020_supp != BMD_TRUE) {
                                LOG(LOG_LEVEL_WARNING)
                                    << MOD_NAME
                                    << "Rec. 2020 color space metadata not supported.\n";
                                }
                }
        }

        if (s->play_audio) {
                /* Actually no action is required to set audio connection because Blackmagic card
                 * plays audio through all its outputs (AES/SDI/analog) ....
                 */
                LOG(LOG_LEVEL_INFO) << MOD_NAME "Audio output set to: "
                                    << bmd_get_audio_connection_name(audioConnection) << "\n";
                /*
                 * .... one exception is a card that has switchable cables between AES/EBU and
                 * analog. (But this applies only for channels 3 and above.)
                 */
                if (audioConnection != 0) { // we will set switchable AESEBU or analog
                        result = deckLinkConfiguration->SetInt(
                            bmdDeckLinkConfigAudioOutputAESAnalogSwitch, audioConnection);
                        if (result == S_OK) { // has switchable channels
                                log_msg(LOG_LEVEL_INFO,
                                        MOD_NAME "Card with switchable audio channels detected. "
                                                 "Switched to correct format.\n");
                        } else if (result == E_NOTIMPL) {
                                // normal case - without switchable channels
                        } else {
                                log_msg(LOG_LEVEL_WARNING,
                                        MOD_NAME "Unable to switch audio output for channels 3 or "
                                                 "above although \n"
                                                 "card shall support it. Check if it is ok. "
                                                 "Continuing anyway.\n");
                        }
                }
        }

        if (!s->low_latency) {
                s->delegate.SetDecklinkOutput(s->deckLinkOutput);
        }
        // s->state.at(i).deckLinkOutput->DisableAudioOutput();

        return (void *)s;
}

static void display_decklink_done(void *state)
{
        debug_msg("display_decklink_done\n"); /* TOREMOVE */
        struct state_decklink *s = (struct state_decklink *)state;

        assert (s != NULL);

        if (s->initialized) {
                if (!s->low_latency) {
                        CALL_AND_CHECK(
                                    s->deckLinkOutput->StopScheduledPlayback(0, nullptr, 0),
                                    "StopScheduledPlayback");
                }
                s->deckLinkOutput->SetScheduledFrameCompletionCallback(nullptr);
                if (s->play_audio) {
                        CALL_AND_CHECK(
                                    s->deckLinkOutput->DisableAudioOutput(),
                                    "DisableAudiioOutput");
                }
                CALL_AND_CHECK(s->deckLinkOutput->DisableVideoOutput(),
                               "DisableVideoOutput");
        }

        RELEASE_IF_NOT_NULL(s->deckLinkAttributes);
        RELEASE_IF_NOT_NULL(s->deckLinkConfiguration);
        RELEASE_IF_NOT_NULL(s->deckLinkOutput);
        RELEASE_IF_NOT_NULL(s->deckLink);

        while (!s->buffer_pool.frame_queue.empty()) {
                auto tmp = s->buffer_pool.frame_queue.front();
                s->buffer_pool.frame_queue.pop();
                tmp->Release();
        }

        delete s->timecode;

        decklink_uninitialize(&s->com_initialized);
        delete s;
}

static bool display_decklink_get_property(void *state, int property, void *val, size_t *len)
{
        struct state_decklink *s = (struct state_decklink *)state;
        vector<codec_t> codecs(uv_to_bmd_codec_map.size());
        int rgb_shift[] = {16, 8, 0};
        interlacing_t supported_il_modes[] = {PROGRESSIVE, INTERLACED_MERGED, SEGMENTED_FRAME};
        int count = 0;
        for (auto & c : uv_to_bmd_codec_map) {
                if (decklink_supports_codec(s->deckLinkOutput, c.second)) {
                        codecs[count++] = c.first;
                }
        }
        
        switch (property) {
                case DISPLAY_PROPERTY_CODECS:
                        if(sizeof(codec_t) * count <= *len) {
                                memcpy(val, codecs.data(), sizeof(codec_t) * count);
                                *len = sizeof(codec_t) * count;
                        } else {
                                return false;
                        }
                        break;
                case DISPLAY_PROPERTY_RGB_SHIFT:
                        if(sizeof(rgb_shift) > *len) {
                                return false;
                        }
                        memcpy(val, rgb_shift, sizeof(rgb_shift));
                        *len = sizeof(rgb_shift);
                        break;
                case DISPLAY_PROPERTY_BUF_PITCH:
                        *(int *) val = PITCH_DEFAULT;
                        *len = sizeof(int);
                        break;
                case DISPLAY_PROPERTY_VIDEO_MODE:
                        *(int *) val = DISPLAY_PROPERTY_VIDEO_SEPARATE_3D;
                        *len = sizeof(int);
                        break;
                case DISPLAY_PROPERTY_SUPPORTED_IL_MODES:
                        if(sizeof(supported_il_modes) <= *len) {
                                memcpy(val, supported_il_modes, sizeof(supported_il_modes));
                        } else {
                                return false;
                        }
                        *len = sizeof(supported_il_modes);
                        break;
                case DISPLAY_PROPERTY_AUDIO_FORMAT:
                        {
                                assert(*len == sizeof(struct audio_desc));
                                struct audio_desc *desc = (struct audio_desc *) val;
                                desc->sample_rate = 48000;
                                if (desc->ch_count >= s->max_aud_chans) {
                                        desc->ch_count = (int) s->max_aud_chans;
                                } else if (desc->ch_count <= 2) {
                                        desc->ch_count = 2;
                                } else if (desc->ch_count <= 8) {
                                        desc->ch_count = 8;
                                } else {
                                        desc->ch_count =
                                            next_power_of_two(desc->ch_count);
                                }
                                desc->codec = AC_PCM;
                                desc->bps = desc->bps < 3 ? 2 : 4;
                        }
                        break;
                default:
                        return false;
        }
        return true;
}

/*
 * AUDIO
 */
void PlaybackDelegate::ScheduleAudio(const struct audio_frame *frame,
                                     uint32_t *const samples) {
        if (m_adata.saved_sync_ts == INT64_MIN &&
            m_audio_sync_ts == audio_sync_val::deinit) {
                        return;
        }
        if (m_adata.saved_sync_ts != m_audio_sync_ts &&
            m_audio_sync_ts > audio_sync_val::resync) {
                m_adata = audio_vals{};
                m_adata.last_sync_ts =
                    m_adata.saved_sync_ts = m_audio_sync_ts;
        }

        if (frame->timestamp < m_adata.last_sync_ts) { // wrap-around
                m_adata.last_sync_ts -= (1LLU << 32);
        }
        BMDTimeValue streamTime =
            ((int64_t) frame->timestamp - m_adata.last_sync_ts) *
            bmdAudioSampleRate48kHz / 90000;

        LOG(LOG_LEVEL_DEBUG) << MOD_NAME << "streamTime: " << streamTime
                             << "; samples: " << *samples
                             << "; RTP timestamp: " << frame->timestamp
                             << "; sync TS: " << m_audio_sync_ts << "\n";
        const HRESULT res = m_deckLinkOutput->ScheduleAudioSamples(
            frame->data, *samples, streamTime, bmdAudioSampleRate48kHz,
            samples);
        if (FAILED(res)) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "ScheduleAudioSamples: "
                                     << bmd_hresult_to_string(res) << "\n";
        }
}

static void display_decklink_put_audio_frame(void *state, const struct audio_frame *frame)
{
        struct state_decklink *s = (struct state_decklink *)state;
        assert(s->play_audio);
        if (s->audio_reconfigure) {
                return;
        }
        const unsigned int sampleFrameCount =
            frame->data_len / (frame->bps * frame->ch_count);
        uint32_t sampleFramesWritten = sampleFrameCount;

        uint32_t buffered = 0;
        s->deckLinkOutput->GetBufferedAudioSampleFrameCount(&buffered);
        if (buffered == 0) {
                LOG(LOG_LEVEL_WARNING) << MOD_NAME << "audio buffer underflow!\n";
        }

        if (s->low_latency) {
                HRESULT res = s->deckLinkOutput->WriteAudioSamplesSync(frame->data, sampleFrameCount,
                                &sampleFramesWritten);
                if (FAILED(res)) {
                        log_msg(LOG_LEVEL_WARNING, MOD_NAME "WriteAudioSamplesSync failed.\n");
                        return;
                }
        } else {
                s->delegate.ScheduleAudio(frame, &sampleFramesWritten);
        }
        const bool overflow = sampleFramesWritten != sampleFrameCount;
        if (overflow || log_level >= LOG_LEVEL_DEBUG) {
                ostringstream details_oss;
                if (log_level >= LOG_LEVEL_VERBOSE) {
                        details_oss
                            << " (" << sampleFramesWritten << " written, "
                            << sampleFrameCount - sampleFramesWritten
                            << " dropped, " << buffered << " buffer size)";
                }
                int level = overflow ? LOG_LEVEL_WARNING : LOG_LEVEL_DEBUG;
                LOG(level) << MOD_NAME << "audio buffer"
                           << (overflow ? " overflow!" : "")
                           << details_oss.str() << "\n";
        }
        s->audio_drift_fixer.update(buffered, sampleFrameCount, sampleFramesWritten);
}

static bool display_decklink_reconfigure_audio(void *state, int quant_samples, int channels,
                int sample_rate) {
        struct state_decklink *s = (struct state_decklink *)state;

        assert(s->play_audio);
        assert(channels >= 2 && channels != 4 && channels <= 64 &&
               is_power_of_two(channels));
        assert(quant_samples == 16 || quant_samples == 32);
        
        const int bps = quant_samples / 8;
        if (bps != s->aud_desc.bps || sample_rate != s->aud_desc.sample_rate ||
            channels != s->aud_desc.ch_count) {
                const unique_lock<mutex> lk(s->audio_reconf_lock);
                s->aud_desc = {quant_samples / 8, sample_rate, channels,
                               AC_PCM};
                s->audio_reconfigure = true;
                LOG(LOG_LEVEL_VERBOSE)
                    << MOD_NAME << "Audio reconfigured to: " << s->aud_desc
                    << "\n";
        }

        return true;
}

#ifndef WIN32
static bool operator==(const REFIID & first, const REFIID & second){
    return (first.byte0 == second.byte0) &&
        (first.byte1 == second.byte1) &&
        (first.byte2 == second.byte2) &&
        (first.byte3 == second.byte3) &&
        (first.byte4 == second.byte4) &&
        (first.byte5 == second.byte5) &&
        (first.byte6 == second.byte6) &&
        (first.byte7 == second.byte7) &&
        (first.byte8 == second.byte8) &&
        (first.byte9 == second.byte9) &&
        (first.byte10 == second.byte10) &&
        (first.byte11 == second.byte11) &&
        (first.byte12 == second.byte12) &&
        (first.byte13 == second.byte13) &&
        (first.byte14 == second.byte14) &&
        (first.byte15 == second.byte15);
}
#endif

HRESULT DeckLinkFrame::QueryInterface(REFIID iid, LPVOID *ppv)
{
#ifdef _WIN32
        IID                     iunknown = IID_IUnknown;
#else
        CFUUIDBytes             iunknown = CFUUIDGetUUIDBytes(IUnknownUUID);
#endif
        HRESULT                 result          = S_OK;

        if (ppv == nullptr) {
                return E_INVALIDARG;
        }

        // Initialise the return result
        *ppv = nullptr;

        LOG(LOG_LEVEL_DEBUG) << MOD_NAME << "DeckLinkFrame QueryInterface " << iid << "\n";
        if (iid == iunknown) {
                *ppv = this;
                AddRef();
        } else if (iid == IID_IDeckLinkVideoFrame) {
                *ppv = static_cast<IDeckLinkVideoFrame*>(this);
                AddRef();
        } else if (iid == IID_IDeckLinkVideoFrameMetadataExtensions) {
                if (m_metadata.EOTF == static_cast<int64_t>(HDR_EOTF::NONE)) {
                        result = E_NOINTERFACE;
                } else {
                        *ppv = static_cast<IDeckLinkVideoFrameMetadataExtensions*>(this);
                        AddRef();
                }
        } else {
                result = E_NOINTERFACE;
        }

        return result;

        return E_NOINTERFACE;
}

ULONG DeckLinkFrame::AddRef()
{
        return ++ref;
}

ULONG DeckLinkFrame::Release()
{
        ULONG ret = --ref;
        if (ret == 0) {
                lock_guard<mutex> lg(buffer_pool.lock);
                buffer_pool.frame_queue.push(this);
        }
	return ret;
}

DeckLinkFrame::DeckLinkFrame(long w, long h, long rb, BMDPixelFormat pf, buffer_pool_t & bp, HDRMetadata const & hdr_metadata)
	: width(w), height(h), rawBytes(rb), pixelFormat(pf), data(new char[rb * h]), timecode(NULL),
        buffer_pool(bp)
{
        clear_video_buffer(reinterpret_cast<unsigned char *>(data.get()), rawBytes, rawBytes, height,
                        pf == bmdFormat8BitYUV ? UYVY : (pf == bmdFormat10BitYUV ? v210 : RGBA));
        m_metadata = hdr_metadata;
}

DeckLinkFrame *DeckLinkFrame::Create(long width, long height, long rawBytes, BMDPixelFormat pixelFormat, buffer_pool_t & buffer_pool, const HDRMetadata & hdr_metadata)
{
        return new DeckLinkFrame(width, height, rawBytes, pixelFormat, buffer_pool, hdr_metadata);
}

DeckLinkFrame::~DeckLinkFrame() 
{
}

long DeckLinkFrame::GetWidth ()
{
        return width;
}

long DeckLinkFrame::GetHeight ()
{
        return height;
}

long DeckLinkFrame::GetRowBytes ()
{
        return rawBytes;
}

BMDPixelFormat DeckLinkFrame::GetPixelFormat ()
{
        return pixelFormat;
}

BMDFrameFlags DeckLinkFrame::GetFlags ()
{
        return m_metadata.EOTF == static_cast<int64_t>(HDR_EOTF::NONE) ? bmdFrameFlagDefault : bmdFrameContainsHDRMetadata;
}

HRESULT DeckLinkFrame::GetBytes (/* out */ void **buffer)
{
        *buffer = static_cast<void *>(data.get());
        return S_OK;
}

HRESULT DeckLinkFrame::GetTimecode (/* in */ BMDTimecodeFormat, /* out */ IDeckLinkTimecode **timecode)
{
        *timecode = dynamic_cast<IDeckLinkTimecode *>(this->timecode);
        return S_OK;
}

HRESULT DeckLinkFrame::GetAncillaryData (/* out */ IDeckLinkVideoFrameAncillary **)
{
	return S_FALSE;
}

/* IDeckLinkMutableVideoFrame */
HRESULT DeckLinkFrame::SetFlags (/* in */ BMDFrameFlags)
{
        return E_FAIL;
}

HRESULT DeckLinkFrame::SetTimecode (/* in */ BMDTimecodeFormat, /* in */ IDeckLinkTimecode *timecode)
{
        if(this->timecode)
                this->timecode->Release();
        this->timecode = timecode;
        return S_OK;
}

HRESULT DeckLinkFrame::SetTimecodeFromComponents (/* in */ BMDTimecodeFormat, /* in */ uint8_t, /* in */ uint8_t, /* in */ uint8_t, /* in */ uint8_t, /* in */ BMDTimecodeFlags)
{
        return E_FAIL;
}

HRESULT DeckLinkFrame::SetAncillaryData (/* in */ IDeckLinkVideoFrameAncillary *)
{
        return E_FAIL;
}

HRESULT DeckLinkFrame::SetTimecodeUserBits (/* in */ BMDTimecodeFormat, /* in */ BMDTimecodeUserBits)
{
        return E_FAIL;
}

void HDRMetadata::Init(const string &fmt) {
        auto opts = unique_ptr<char []>(new char [fmt.size() + 1]);
        strcpy(opts.get(), fmt.c_str());
        char *save_ptr = nullptr;
        char *mode_c = strtok_r(opts.get(), ",", &save_ptr);
        assert(mode_c != nullptr);
        string mode = mode_c;
        std::for_each(std::begin(mode), std::end(mode), [](char& c) {
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        });
        if (mode == "SDR"s) {
                EOTF = static_cast<int64_t>(HDR_EOTF::SDR);
        } else if (mode == "HDR"s) {
                EOTF = static_cast<int64_t>(HDR_EOTF::HDR);
        } else if (mode == "PQ"s) {
                EOTF = static_cast<int64_t>(HDR_EOTF::PQ);
        } else if (mode == "HLG"s) {
                EOTF = static_cast<int64_t>(HDR_EOTF::HLG);
        } else if (mode == "HELP"s) {
                cout << MOD_NAME << "HDR syntax:\n";
                cout << "\tHDR[=<eotf>|int[,{<k>=<v>}*]\n";
                cout << "\t\t<eotf> may be one of SDR, HDR, PQ, HLG or int 0-7\n";
                cout << "\t\tFurther options may be specification of HDR values, accepted keys are (values are floats):\n";
                cout << "\t\t\t- maxDisplayMasteringLuminance\n";
                cout << "\t\t\t- minDisplayMasteringLuminance\n";
                cout << "\t\t\t- maxCLL\n";
                cout << "\t\t\t- maxFALL\n";
                throw ug_no_error{};
        } else {
                EOTF = stoi(mode);
                if (EOTF < 0 || EOTF > 7) {
                        throw out_of_range("Value outside [0..7]");
                }
        }

        char *other_opt = nullptr;
        while ((other_opt = strtok_r(nullptr, ",", &save_ptr)) != nullptr) {
                if (strstr(other_opt, "maxDisplayMasteringLuminance=") != nullptr) {
                        maxDisplayMasteringLuminance = stod(other_opt + strlen("maxDisplayMasteringLuminance="));
                } else if (strstr(other_opt, "minDisplayMasteringLuminance=") != nullptr) {
                        minDisplayMasteringLuminance = stod(other_opt + strlen("minDisplayMasteringLuminance="));
                } else if (strstr(other_opt, "maxCLL=") != nullptr) {
                        maxCLL = stod(other_opt + strlen("maxCLL="));
                } else if (strstr(other_opt, "maxFALL=") != nullptr) {
                        maxFALL = stod(other_opt + strlen("maxFALL="));
                } else {
                        throw invalid_argument("Unrecognized HDR attribute "s + other_opt);
                }
        }
}

static inline void debug_print_metadata_id(const char *fn_name, BMDDeckLinkFrameMetadataID metadataID) {
        if (log_level < LOG_LEVEL_DEBUG2) {
                return;
        }
        array<char, sizeof metadataID + 1> fourcc{};
        copy(reinterpret_cast<char *>(&metadataID), reinterpret_cast<char *>(&metadataID) + sizeof metadataID, fourcc.data());
        LOG(LOG_LEVEL_DEBUG2) << MOD_NAME << "DecklLinkFrame " << fn_name << ": " << fourcc.data() << "\n";
}

// IDeckLinkVideoFrameMetadataExtensions interface
HRESULT DeckLinkFrame::GetInt(BMDDeckLinkFrameMetadataID metadataID, int64_t* value)
{
        debug_print_metadata_id(static_cast<const char *>(__func__), metadataID);
        switch (metadataID)
        {
                case bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc:
                        *value = m_metadata.EOTF;
                        return S_OK;
                case bmdDeckLinkFrameMetadataColorspace:
                        // Colorspace is fixed for this sample
                        *value = bmdColorspaceRec2020;
                        return S_OK;
                default:
                        value = nullptr;
                        return E_INVALIDARG;
        }
}

HRESULT DeckLinkFrame::GetFloat(BMDDeckLinkFrameMetadataID metadataID, double* value)
{
        debug_print_metadata_id(static_cast<const char *>(__func__), metadataID);
        switch (metadataID)
        {
                case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX:
                        *value = m_metadata.referencePrimaries.RedX;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY:
                        *value = m_metadata.referencePrimaries.RedY;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX:
                        *value = m_metadata.referencePrimaries.GreenX;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY:
                        *value = m_metadata.referencePrimaries.GreenY;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX:
                        *value = m_metadata.referencePrimaries.BlueX;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY:
                        *value = m_metadata.referencePrimaries.BlueY;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRWhitePointX:
                        *value = m_metadata.referencePrimaries.WhiteX;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRWhitePointY:
                        *value = m_metadata.referencePrimaries.WhiteY;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance:
                        *value = m_metadata.maxDisplayMasteringLuminance;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance:
                        *value = m_metadata.minDisplayMasteringLuminance;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel:
                        *value = m_metadata.maxCLL;
                        return S_OK;
                case bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel:
                        *value = m_metadata.maxFALL;
                        return S_OK;
                default:
                        value = nullptr;
                        return E_INVALIDARG;
        }
}

HRESULT DeckLinkFrame::GetFlag(BMDDeckLinkFrameMetadataID metadataID, BMD_BOOL* value)
{
        debug_print_metadata_id(static_cast<const char *>(__func__), metadataID);
        // Not expecting GetFlag
        *value = BMD_TRUE;
        return E_INVALIDARG;
}

HRESULT DeckLinkFrame::GetString(BMDDeckLinkFrameMetadataID metadataID, BMD_STR* value)
{
        debug_print_metadata_id(static_cast<const char *>(__func__), metadataID);
        // Not expecting GetString
        *value = nullptr;
        return E_INVALIDARG;
}

HRESULT DeckLinkFrame::GetBytes(BMDDeckLinkFrameMetadataID metadataID, void* /* buffer */, uint32_t* bufferSize)
{
        debug_print_metadata_id(static_cast<const char *>(__func__), metadataID);
        *bufferSize = 0;
        return E_INVALIDARG;
}

// 3D frame
DeckLink3DFrame::DeckLink3DFrame(long w, long h, long rb, BMDPixelFormat pf, buffer_pool_t & buffer_pool, HDRMetadata const & hdr_metadata)
        : DeckLinkFrame(w, h, rb, pf, buffer_pool, hdr_metadata), rightEye(DeckLinkFrame::Create(w, h, rb, pf, buffer_pool, hdr_metadata))
{
}

DeckLink3DFrame *DeckLink3DFrame::Create(long width, long height, long rawBytes, BMDPixelFormat pixelFormat, buffer_pool_t & buffer_pool, HDRMetadata const & hdr_metadata)
{
        DeckLink3DFrame *frame = new DeckLink3DFrame(width, height, rawBytes, pixelFormat, buffer_pool, hdr_metadata);
        return frame;
}

DeckLink3DFrame::~DeckLink3DFrame()
{
}

ULONG DeckLink3DFrame::AddRef()
{
        return DeckLinkFrame::AddRef();
}

ULONG DeckLink3DFrame::Release()
{
        return DeckLinkFrame::Release();
}

HRESULT DeckLink3DFrame::QueryInterface(REFIID id, void **data)
{
        LOG(LOG_LEVEL_DEBUG) << MOD_NAME << "DecklLink3DFrame QueryInterface " << id << "\n";
        if(id == IID_IDeckLinkVideoFrame3DExtensions)
        {
                this->AddRef();
                *data = dynamic_cast<IDeckLinkVideoFrame3DExtensions *>(this);
                return S_OK;
        }
        return DeckLinkFrame::QueryInterface(id, data);
}

BMDVideo3DPackingFormat DeckLink3DFrame::Get3DPackingFormat()
{
        return bmdVideo3DPackingLeftOnly;
}

HRESULT DeckLink3DFrame::GetFrameForRightEye(IDeckLinkVideoFrame ** frame) 
{
        *frame = rightEye.get();
        rightEye->AddRef();
        return S_OK;
}

/* function from DeckLink SDK sample DeviceList */
static void
print_output_modes(IDeckLink *deckLink, const char *query_prop_fcc)
{
        IDeckLinkOutput*                        deckLinkOutput = NULL;
        IDeckLinkDisplayModeIterator*           displayModeIterator = NULL;
        IDeckLinkDisplayMode*                   displayMode = NULL;
        HRESULT                                 result;
        int                                     displayModeNumber = 0;

        // Query the DeckLink for its configuration interface
        result = deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&deckLinkOutput);
        if (result != S_OK)
        {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Could not obtain the IDeckLinkOutput interface - result = %08x\n", (int) result);
                if (result == E_NOINTERFACE) {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "Device doesn't support video playback.\n");
                }
                goto bail;
        }

        // Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
        result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
        if (result != S_OK)
        {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Could not obtain the video output display mode iterator - result = %08x\n", (int) result);
                goto bail;
        }

        // List all supported output display modes
        printf("\tdisplay modes:\n");
        while (displayModeIterator->Next(&displayMode) == S_OK)
        {
                BMD_STR                  displayModeString = NULL;

                result = displayMode->GetName(&displayModeString);

                if (result == S_OK)
                {
                        char *displayModeCString = get_cstr_from_bmd_api_str(displayModeString);
                        BMDTimeValue    frameRateDuration;
                        BMDTimeScale    frameRateScale;

                        // Obtain the display mode's properties
                        string flags_str = bmd_get_flags_str(displayMode->GetFlags());
                        int modeWidth = displayMode->GetWidth();
                        int modeHeight = displayMode->GetHeight();
                        uint32_t field_dominance_n = ntohl(displayMode->GetFieldDominance());
                        displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
                        printf("\t\t%2d) %-20s  %d x %d \t %2.2f FPS %.4s, flags: %s\n",displayModeNumber, displayModeCString,
                                        modeWidth, modeHeight, (float) ((double)frameRateScale / (double)frameRateDuration),
                                        (char *) &field_dominance_n, flags_str.c_str());
                        release_bmd_api_str(displayModeString);
                        free(displayModeCString);
                }

                // Release the IDeckLinkDisplayMode object to prevent a leak
                displayMode->Release();

                displayModeNumber++;
        }
        color_printf("\n\tsupported pixel formats:" TERM_BOLD);
        for (auto & c : uv_to_bmd_codec_map) {
                if (decklink_supports_codec(deckLinkOutput, c.second)) {
                        printf(" %s", get_codec_name(c.first));
                }
        }
        color_printf(TERM_RESET "\n");

        if (query_prop_fcc != nullptr) {
                IDeckLinkProfileAttributes *deckLinkAttributes = nullptr;
                if (deckLink->QueryInterface(IID_IDeckLinkProfileAttributes,
                                             (void **) &deckLinkAttributes) ==
                    S_OK) {
                        cout << "\n";
                        print_bmd_attribute(deckLinkAttributes, query_prop_fcc);
                        deckLinkAttributes->Release();
                } else {
                        MSG(ERROR, "Could not query device attributes.\n\n");
                }
        }

        color_printf("\n");

bail:
        // Ensure that the interfaces we obtained are released to prevent a memory leak
        if (displayModeIterator != NULL)
                displayModeIterator->Release();

        if (deckLinkOutput != NULL)
                deckLinkOutput->Release();
}

static const struct video_display_info display_decklink_info = {
        display_decklink_probe,
        display_decklink_init,
        NULL, // _run
        display_decklink_done,
        display_decklink_getf,
        display_decklink_putf,
        display_decklink_reconfigure,
        display_decklink_get_property,
        display_decklink_put_audio_frame,
        display_decklink_reconfigure_audio,
        MOD_NAME,
};

REGISTER_MODULE(decklink, &display_decklink_info, LIBRARY_CLASS_VIDEO_DISPLAY, VIDEO_DISPLAY_ABI_VERSION);

/* vim: set expandtab sw=8: */
