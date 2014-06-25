/*******************************************************************************
 * AVPacket: demux后但未解码的数据. 由av_read_frame()读取.
 * AVFrame:  decode后的帧数据. 需要先有av_frame_alloc()分配.
*******************************************************************************/

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include "libavutil/avstring.h"
#include "libavutil/colorspace.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#if CONFIG_AVFILTER
# include "libavfilter/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/avfiltergraph.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"

#include <assert.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;  // All packets' size in bytes in the list.
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


const char program_name[] = "ffplay";
const int program_birth_year = 2003;
int quit = 0;
PacketQueue audioq;
struct SwrContext *swr_ctx = NULL;

void show_help_default(const char *opt, const char *arg)
{
}

static void packet_queue_init(PacketQueue *q)
{
  memset(q, 0, sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCond();
}

// Put pkt at tail(last_pkt) of q
static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    if(av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;


    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

// Get pkt from queue's head(first_pkt)
// block: [out] If TRUE, blocks and waits until got pkt, else returns 0 immediatelly.
// return: 1: Got pkt; 0: No pkt in the queue yet.
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);
    for(;;) {
        if(quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// ffmpeg interrupt.
static int interrupt_callback_simple(void *ctx)
{
    return quit;
}

/*
 * audio_buf[out]: 解码后的音频帧的数据
 * buf_size [in] : audio_buf的大小.
 */
static int audio_decode_frame(AVCodecContext *a_codec_ctx, uint8_t *audio_buf, int buf_size)
{
    static AVPacket pkt;                // 未解码的数据             // TODO: remove static
    static int pkt_2be_decode_size = 0; // 未解码的数据大小, 第一次进入是为0, 表示要先跳到函数最后去获取pkt数据  // TODO: remove static
    AVFrame *frame;                     // 用于存放解码后的数据
    int pkt_consumed_size;              // Decoded(consumed) packet size
    int frame_data_size = 0;            // Decoded frame data size
    int got_frame;
    uint8_t *data_decoded;
    uint8_t *pout = audio_buf;
    int total_size = 0;

    memset(&pkt, 0, sizeof(pkt));
    frame = av_frame_alloc();
    if(frame == NULL)
        return -1;

    for(;;) {
        while(pkt_2be_decode_size > 0) {  // Decode pkt and resample
            // Decode
            pkt_consumed_size = avcodec_decode_audio4(a_codec_ctx, frame, &got_frame, &pkt);
            av_log(NULL, AV_LOG_DEBUG, "pkt_consumed_size = %d\n", pkt_consumed_size);
            if(pkt_consumed_size < 0 || !got_frame) { /* if error, skip frame */
                pkt_2be_decode_size = 0;
                break;
            }

            frame_data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(frame),
                                                   frame->nb_samples,
                                                   frame->format, 1);
            if(frame_data_size <= 0) {
                av_log(NULL, AV_LOG_WARNING, "frame_data_size = %d, chs = %d, nb_samples = %d, format = %d.\n",
                        frame_data_size, av_frame_get_channels(frame), frame->nb_samples, frame->format);
                continue; /* No data yet, get more frames */
            }

            // swr {{{
            if (frame->format != AV_SAMPLE_FMT_S16) { // != is->audio_src.fmt
                av_log(NULL, AV_LOG_WARNING, "frame->format   !=  AV_SAMPLE_FMT_S16\n");
            }

//            av_log(NULL, AV_LOG_INFO, "swr_ctx = %p\n", swr_ctx);
            if (swr_ctx) {
                uint8_t *swr_out_buffer;
                uint8_t *swr_out[1];
                unsigned int swr_buffer_size;
                int swr_out_samples_per_ch;
                int resampled_data_size;

                int swr_buffer_want_size  = av_samples_get_buffer_size(NULL, frame->channels, frame->nb_samples, AV_SAMPLE_FMT_S16, 0);
                swr_out_buffer = av_malloc(swr_buffer_want_size);
                swr_buffer_size = swr_buffer_want_size;
                av_log(NULL, AV_LOG_DEBUG, "frame->channels = %d, frame->channels = %d, swr_buffer_want_size = %d, swr_buffer_size = %d\n",
                        frame->channels, frame->nb_samples, swr_buffer_want_size, swr_buffer_size);
                if (!swr_out_buffer)
                    return AVERROR(ENOMEM);
                swr_out[0] = swr_out_buffer;
                swr_out_samples_per_ch = swr_convert(swr_ctx, swr_out, frame->nb_samples, frame->data, frame->nb_samples);
                if (swr_out_samples_per_ch < 0) {
                    fprintf(stderr, "swr convert() failed\n");
                    return -1;
                }
                if (swr_out_samples_per_ch == swr_buffer_size) {
                    fprintf(stderr, "warning: audio buffer is probably too small\n");
                    swr_init(swr_ctx);
                }
                frame_data_size = swr_out_samples_per_ch * frame->channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
                data_decoded = swr_out_buffer;
                av_log(NULL, AV_LOG_DEBUG, "swr_out_samples_per_ch = %d, frame_data_size = %d\n", swr_out_samples_per_ch, frame_data_size);
            }
            else {
                data_decoded = frame->data[0];
            }
            // swr }}}

            memcpy(pout, data_decoded, frame_data_size);
            av_log(NULL, AV_LOG_DEBUG, "buf_size = %d, frame_data_size = %d, pkt.size =  %d, frame data = %02x %02x %02x %02x, %02x %02x %02x %02x\n",
                                        buf_size,      frame_data_size,      pkt.size,       data_decoded[0], data_decoded[1], data_decoded[2], data_decoded[3], data_decoded[4], data_decoded[6], data_decoded[7], data_decoded[8]);
            pkt_2be_decode_size -= pkt_consumed_size;
            pout += frame_data_size;
            total_size += frame_data_size;
            /* We have data, return it and come back for more later */
            if(got_frame) {
                av_log(NULL, AV_LOG_DEBUG, "got frame.\n");
                return total_size;
            }
        }
        if(pkt.data)
            av_free_packet(&pkt);

        if(quit) {
            av_free (frame);
            av_log(NULL, AV_LOG_DEBUG, "quit\n");
            return -1;
        }

        // Get pkt(undecoded) from the queue
        if(packet_queue_get(&audioq, &pkt, 1) < 0) {
            av_free (frame);
            av_log(NULL, AV_LOG_ERROR, "packet_queue_get error\n");
            return -1;
        }
        av_log(NULL, AV_LOG_DEBUG, "read packet from queue, pkt.size = %d\n", pkt.size);
        pkt_2be_decode_size = pkt.size;
    }

    av_free (frame);
    return 0;
}

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    AVCodecContext *a_codec_ctx = (AVCodecContext *)userdata;
    int len1, audio_size;
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    av_log(NULL, AV_LOG_DEBUG, "audio_callback enter, needed len = %d\n", len);

    while(len > 0) {                            // 仍未满足SDL需求数据量, 需继续向解码器要数据.
        av_log(NULL, AV_LOG_DEBUG, "len > 0\n");
        if(audio_buf_index >= audio_buf_size) { // 上一次解码出来的数据已经推完, 需继续解码.
            audio_size = audio_decode_frame(a_codec_ctx, audio_buf, sizeof(audio_buf));
            if(audio_size < 0) {                // If error, output silence
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            av_log(NULL, AV_LOG_DEBUG, "audio_decode_frame returns %d\n", audio_size);
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if(len1 > len)
            len1 = len;
        
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1); // 将数据推给SDL.

        len -= len1;
        av_log(NULL, AV_LOG_DEBUG, "copy data to SDL: p = %p, copyed_len = %d, remain_len = %d, data = %02x%02x%02x%02x%02x%02x%02x%02x\n", audio_buf + audio_buf_index, len1, len,
                stream[0], stream[1], stream[2], stream[3], stream[5], stream[6], stream[7], stream[8]);
        stream += len1;
        audio_buf_index += len1;

        if(quit) {
            return;
        }
    }
    av_log(NULL, AV_LOG_INFO, "audio_callback return.\n");
}

int main(int argc, char **argv)
{
    AVFormatContext *format_ctx = NULL;
    int err, i;
    char *filename = (char *)"alan.mp4"; // argv[1];
    AVCodec *v_codec = NULL;
    AVCodec *a_codec = NULL;
    AVCodecContext *v_codec_ctx;
    AVCodecContext *a_codec_ctx;
    AVFrame *frame;  // 用于存放解码后的数据
    uint8_t *buffer;
    int frame_finished;
    AVPacket packet;
    int v_stream_index;
    int a_stream_index;
    struct SwsContext *sws_ctx; 
    SDL_AudioSpec sdl_audio_spec;
    SDL_Surface *screen;
    SDL_Overlay *bmp;
    SDL_Rect    rect;
    SDL_Event   event;
    AVPicture   pict;
    int64_t a_ch_layout;

    // Program init
    av_log_set_level(AV_LOG_VERBOSE);
    av_log(NULL, AV_LOG_INFO, "Playing: %s\n", filename);
    av_register_all();

    // Open stream file
    format_ctx = avformat_alloc_context();
    format_ctx->interrupt_callback.callback = interrupt_callback_simple;
    format_ctx->interrupt_callback.opaque = NULL;
    err = avformat_open_input(&format_ctx, argv[1], NULL, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: avformat_open_input fails, ret = %d\n", filename, err);
        return -1;
    }

    // Parse metadata
    err = avformat_find_stream_info(format_ctx, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", filename);
        return -1;
    }

    av_dump_format(format_ctx, 0, filename, 0);

    // Find a/v decoder's stream index.
    av_log(NULL, AV_LOG_INFO, "nb_streams in %s = %d\n", filename, format_ctx->nb_streams);
    v_stream_index = -1;
    a_stream_index = -1;
    v_codec_ctx = NULL;
    a_codec_ctx = NULL;
    for (i = 0; i < format_ctx->nb_streams; i++) {
        if(format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            v_stream_index=i;
            v_codec_ctx=format_ctx->streams[i]->codec;
            av_log(NULL, AV_LOG_DEBUG, "video stream index = %d\n", i, format_ctx->streams[i]->codec->codec_type);
        }
        else if(format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && a_stream_index < 0) {
            a_stream_index=i;
            a_codec_ctx=format_ctx->streams[i]->codec;
            av_log(NULL, AV_LOG_DEBUG, "audio stream index = %d\n", i, format_ctx->streams[i]->codec->codec_type);
        }
    }
    if(v_stream_index==-1 && a_stream_index==-1) {
        av_log(NULL, AV_LOG_ERROR, "Haven't find video stream.\n");
        return -1; // Didn't find a video stream
    }

    // Open video decoder stuff
    if(v_codec_ctx) {
        v_codec = avcodec_find_decoder(v_codec_ctx->codec_id);
        if (!v_codec) {
            av_log(NULL, AV_LOG_ERROR, "%s: avcodec_find_decoder fails\n", filename);
            return -1;
        }

        // Open v_codec
        if(avcodec_open2(v_codec_ctx, v_codec, NULL)<0) {
            av_log(NULL, AV_LOG_ERROR, "%s: avcodec_open2 fails\n", filename);
            return -1; // Could not open codec
        }

        // Allocate a/v frame
        frame = av_frame_alloc();
        if(frame == NULL)
            return -1;

        if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
            av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL - %s\n", SDL_GetError());
            exit(1);
        }

        screen = SDL_SetVideoMode(v_codec_ctx->width * 3 / 2, v_codec_ctx->height * 3 / 2, 0, 0);
        if(!screen) {
            av_log(NULL, AV_LOG_ERROR, "SDL: could not set video mode - exiting\n");
            exit(1);
        }

        bmp = SDL_CreateYUVOverlay(v_codec_ctx->width * 3 / 2, v_codec_ctx->height * 3 / 2, SDL_YV12_OVERLAY, screen);

        sws_ctx = sws_getContext (v_codec_ctx->width, v_codec_ctx->height, v_codec_ctx->pix_fmt,
                                  v_codec_ctx->width * 3 / 2, v_codec_ctx->height * 3 / 2, PIX_FMT_YUV420P,
                                  SWS_BICUBIC, NULL, NULL, NULL);
    }

    // Open audio decoder stuff
    if(a_codec_ctx) {
        SDL_AudioSpec spec;
        spec.freq = a_codec_ctx->sample_rate;
        spec.format = AUDIO_S16SYS;
        spec.channels = a_codec_ctx->channels;
        spec.silence = 0;
        spec.samples = SDL_AUDIO_BUFFER_SIZE;
        spec.callback = audio_callback;
        spec.userdata = a_codec_ctx;
        av_log(NULL, AV_LOG_DEBUG, "wanted_spec.ch = %d, wanted_spec.freq = %d\n", spec.channels, spec.freq);
        if(SDL_OpenAudio(&spec, &sdl_audio_spec) < 0) {
            av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }

        a_codec = avcodec_find_decoder(a_codec_ctx->codec_id);
        if(!a_codec) {
          av_log(NULL, AV_LOG_ERROR, "Unsupported audio codec id = %d!\n", a_codec_ctx->codec_id);
          return -1;
        }
        avcodec_open2(a_codec_ctx, a_codec, NULL);

        a_ch_layout = av_get_default_channel_layout(a_codec_ctx->channels);
        swr_ctx = swr_alloc_set_opts(NULL,
                                     a_ch_layout, AV_SAMPLE_FMT_S16, a_codec_ctx->sample_rate,
                                     a_ch_layout, a_codec_ctx->sample_fmt, a_codec_ctx->sample_rate,
                                     0, NULL);
        if (!swr_ctx || swr_init(swr_ctx) < 0) {
            fprintf(stderr, "Cannot create sample rate converter for conversion\n");
            return -1;
        }

        packet_queue_init(&audioq);
        SDL_PauseAudio(0);
    }

    i = 0;
    while(av_read_frame(format_ctx, &packet) >= 0) {
        if(packet.stream_index == v_stream_index) { // Is this a packet from the video stream?
            avcodec_decode_video2(v_codec_ctx, frame, &frame_finished, &packet); // Decode video frame

            if(frame_finished) { // Did we get a video frame?
//                av_log(NULL, AV_LOG_DEBUG, "Frame %d decoding finished. bmp->pitches[0] = %d\n", i, bmp->pitches[0]);
                i++;

                SDL_LockYUVOverlay(bmp);
                pict.data[0] = bmp->pixels[0];
                pict.data[1] = bmp->pixels[2];
                pict.data[2] = bmp->pixels[1];
                pict.linesize[0] = bmp->pitches[0];
                pict.linesize[1] = bmp->pitches[2];
                pict.linesize[2] = bmp->pitches[1];

                // Convert the image into YUV format that SDL uses
                sws_scale(sws_ctx, frame->data, frame->linesize, 0, v_codec_ctx->height, pict.data, pict.linesize);
                SDL_UnlockYUVOverlay(bmp);

                rect.x = 0;
                rect.y = 0;
                rect.w = v_codec_ctx->width * 3 / 2;
                rect.h = v_codec_ctx->height * 3 / 2;
                SDL_DisplayYUVOverlay(bmp, &rect);
            }
            else {
                av_log(NULL, AV_LOG_DEBUG, "Frame not finished.\n");
            }
            av_free_packet(&packet); // Free the packet that was allocated by av_read_frame
        }
        else if(packet.stream_index == a_stream_index) {
            av_log(NULL, AV_LOG_DEBUG, "packet_queue_put\n");
            packet_queue_put(&audioq, &packet);
        }
        else {
            av_free_packet(&packet); // Free the packet that was allocated by av_read_frame
        }
        
        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                quit = 1;
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
    }
    sws_freeContext (sws_ctx);

    if(swr_ctx)
        swr_free(&swr_ctx);

    av_free(frame);
    av_free(buffer);
    avcodec_close(v_codec_ctx);
    av_close_input_file(format_ctx);
}
