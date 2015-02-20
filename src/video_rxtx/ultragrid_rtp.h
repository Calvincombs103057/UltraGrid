/**
 * @file   video_rxtx/ultragrid_rtp.h
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2013-2014 CESNET z.s.p.o.
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

#ifndef VIDEO_RXTX_ULTRAGRID_RTP_H_
#define VIDEO_RXTX_ULTRAGRID_RTP_H_

#include "stats.h"
#include "video_rxtx.h"
#include "video_rxtx/rtp.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

class ultragrid_rtp_video_rxtx : public rtp_video_rxtx {
public:
        ultragrid_rtp_video_rxtx(std::map<std::string, param_u> const &);
        virtual ~ultragrid_rtp_video_rxtx();
        virtual void join();

        // transcoder functions
        friend ssize_t hd_rum_decompress_write(void *state, void *buf, size_t count);
        friend void recompress_process_async(void *state, std::shared_ptr<video_frame> frame);
private:
        static void *receiver_thread(void *arg);
        virtual void send_frame(std::shared_ptr<video_frame>);
        void *receiver_loop();
        static void *send_frame_async_callback(void *arg);
        virtual void send_frame_async(std::shared_ptr<video_frame>);
        virtual void *(*get_receiver_thread())(void *arg);

        void receiver_process_messages();
        void remove_display_from_decoders();
        struct vcodec_state *new_video_decoder();
        static void destroy_video_decoder(void *state);

        struct timeval m_start_time;

        enum video_mode  m_decoder_mode;
        const char      *m_postprocess;
        struct display  *m_display_device;
        const char      *m_requested_encryption;

        /**
         * This variables serve as a notification when asynchronous sending exits
         * @{ */
        bool             m_async_sending;
        std::condition_variable m_async_sending_cv;
        std::mutex       m_async_sending_lock;
        /// @}

        stats<std::chrono::nanoseconds::rep> m_stat_nanoperframeactual;
        std::chrono::steady_clock::time_point m_t0;
        std::chrono::nanoseconds m_duration;
        int m_frames;
};

video_rxtx *create_video_rxtx_ultragrid_rtp(std::map<std::string, param_u> const &params);

#endif // VIDEO_RXTX_ULTRAGRID_RTP_H_

