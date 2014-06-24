#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libswscale/swscale_internal.h"
#include <stdio.h>
#include <SDL.h>
#include <SDL_thread.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio


typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

int quit = 0;

void packet_queue_init(PacketQueue *q) 
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) 
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

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) 
{
	static AVPacket pkt;
	static uint8_t *audio_pkt_data = NULL;
	static int audio_pkt_size = 0;
	AVFrame *pFrame = av_frame_alloc();
	
	int len1, data_size;

	for(;;) {
		while(audio_pkt_size > 0) {
			data_size = buf_size;
			#if 0
			len1 = avcodec_decode_audio4(aCodecCtx, pFrame, &data_size, 
						&pkt);
			#else
			len1 = avcodec_decode_audio3(aCodecCtx, (int16_t *)audio_buf, &data_size, 
						&pkt);
			#endif
			if(len1 < 0) {
				/* if error, skip frame */
				audio_pkt_size = 0;
				break;
			}
			audio_pkt_data += len1;
			audio_pkt_size -= len1;
			if(data_size <= 0) {
				/* No data yet, get more frames */
				continue;
			}
			/* We have data, return it and come back for more later */
			av_frame_free(&pFrame);
			return data_size;
		}
		if(pkt.data)
			av_free_packet(&pkt);

		if(quit) {
			av_frame_free(&pFrame);
			return -1;
		}

		if(packet_queue_get(&audioq, &pkt, 1) < 0) {
			av_frame_free(&pFrame);
			return -1;
		}
		audio_pkt_data = pkt.data;
    		audio_pkt_size = pkt.size;
		printf("\r\n audio_pkt_size:%d!!\r\n",audio_pkt_size);
	}
}

void audio_callback(void *userdata, Uint8 *stream, int len) 
{
	AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
	int len1, audio_size;

	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	printf("\r\n ==============>audio_callback  start\r\n");
	while(len > 0) {
		if(audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
			printf("\r\n audio_size:%d!\r\n", audio_size);
			if(audio_size < 0) {
				/* If error, output silence */
				audio_buf_size = 1024; // arbitrary?
				memset(audio_buf, 0, audio_buf_size);
			} else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		printf("\r\n len1:%d!\r\n", len1);
		printf("\r\n len:%d!\r\n", len);
		if(len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}




int main(int argc, char *argv[])
{
	AVFormatContext *pFormatCtx = NULL;
	int             i, videoStream, audioStream;
	AVCodecContext  *pCodecCtx;
	AVCodec         *pCodec;
	AVFrame         *pFrame; 
	AVPacket        packet;
	int             frameFinished;
	float           aspect_ratio;
	SwsContext *pConvertCtx = NULL;

	AVCodecContext  *aCodecCtx;
	AVCodec         *aCodec = NULL;

	SDL_Overlay     *bmp;
	SDL_Surface     *screen;
	SDL_Rect        rect;
	SDL_Event       event;
	SDL_AudioSpec   wanted_spec, spec;
	
	if(argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		exit(1);
	}
	
	// Register all formats and codecs
	av_register_all();
	printf("\r\n This is so cool!!\r\n");
	
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}
	
	if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
		printf("\r\n Open fail!!\r\n");
		return -1; // Couldn't open file
	} else {
		printf("\r\n Open successful!!\r\n");
	}
		
	// Retrieve stream information
	if(avformat_find_stream_info(pFormatCtx, NULL)<0) {
		printf("\r\n Find stream fail!!\r\n");
 		return -1; // Couldn't find stream information
	} else {
		printf("\r\n Find stream successful!!\r\n");
	}

	av_dump_format(pFormatCtx, 0, argv[1], 0);

	// Find the first video stream
	videoStream = -1;
	audioStream = -1;
 	for(i=0; i<pFormatCtx->nb_streams; i++) {
		if((pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
			&& videoStream < 0) {
			videoStream=i;
		}
		else if((pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
			&& audioStream < 0) {
			audioStream = i;
		}
	}

	if(videoStream == -1) {
		printf("\r\n Didn't find a video stream!!\r\n");
		return -1; // Didn't find a video stream
	}
	if(audioStream == -1) {
		printf("\r\n Didn't find a audio stream!!\r\n");
		return -1; // Didn't find a audio stream
	}

	aCodecCtx=pFormatCtx->streams[audioStream]->codec;
	// Set audio settings from codec info
	wanted_spec.freq = aCodecCtx->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = aCodecCtx->channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = aCodecCtx;

	if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
		fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
		return -1;
  	}

	aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
	if(!aCodec) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}
  
	// Open codec
	if(avcodec_open2(aCodecCtx, aCodec, NULL)<0) {
		fprintf(stderr, "codec can't open!\n");
		return -1; // Could not open codec

	}

	packet_queue_init(&audioq);
	SDL_PauseAudio(0);
  
	// Get a pointer to the codec context for the video stream
	pCodecCtx = pFormatCtx->streams[videoStream]->codec;

	// Find the decoder for the video stream
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if(pCodec == NULL) {
		fprintf(stderr, "Unsupported codec!\n");
  		return -1; // Codec not found
	}

	// Open codec
	if(avcodec_open2(pCodecCtx, pCodec, NULL)<0) {
		fprintf(stderr, "codec can't open!\n");
		return -1; // Could not open codec

	}
		
	// Allocate video frame
	pFrame = av_frame_alloc();
		
	screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 0, 0);
	if(!screen) {
		fprintf(stderr, "SDL: could not set video mode - exiting\n");
		exit(1);
	}

	 // Allocate a place to put our YUV image on that screen
	bmp = SDL_CreateYUVOverlay(pCodecCtx->width, pCodecCtx->height,
							   SDL_YV12_OVERLAY, screen);

	i = 0;
	while(av_read_frame(pFormatCtx, &packet)>=0) {
	  	// Is this a packet from the video stream?
		if(packet.stream_index == videoStream) {
			// Decode video frame
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished,
			                     &packet);
				
			// Did we get a video frame?
			if(frameFinished) {
				SDL_LockYUVOverlay(bmp);
				
				AVPicture pict;
				pict.data[0] = bmp->pixels[0];
				pict.data[1] = bmp->pixels[2];
				pict.data[2] = bmp->pixels[1];
			
				pict.linesize[0] = bmp->pitches[0];
				pict.linesize[1] = bmp->pitches[2];
				pict.linesize[2] = bmp->pitches[1];
				//Convert the image into YUV format that SDL uses
				pConvertCtx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,  
					pCodecCtx->width, pCodecCtx->height,  AV_PIX_FMT_YUV420P,  SWS_BILINEAR, 
					NULL, NULL, NULL);  
				if (pConvertCtx == NULL) {  
					fprintf(stderr, "Cannot initialize the conversion context/n");  
					return -1;
				}  
				
				sws_scale(pConvertCtx, pFrame->data, pFrame->linesize, 0,
					pCodecCtx->height, pict.data, pict.linesize);

				SDL_UnlockYUVOverlay(bmp);
				rect.x = 0;
				rect.y = 0;
				rect.w = pCodecCtx->width;
				rect.h = pCodecCtx->height;
				SDL_DisplayYUVOverlay(bmp, &rect);
				sws_freeContext(pConvertCtx); 
			}
		} 
		else if(packet.stream_index==audioStream) {
			printf("\r\n audioStream!\r\n");
			packet_queue_put(&audioq, &packet);
		} else {
			av_free_packet(&packet);
		}
		//Free the packet that was allocated by av_read_frame
		SDL_PollEvent(&event);
		switch(event.type) {
			case SDL_QUIT:
				printf("\r\n SDL_Quit!\r\n");
				SDL_Quit();
				exit(0);
				break;
			default:
				break;
		}
	}
 	printf("\r\n Read stream End!\r\n");

	// Free the YUV frame
	printf("\r\n av_frame_free!\r\n");
	av_frame_free(&pFrame);

	// Close the codec
	printf("\r\n avcodec_close!\r\n");
	avcodec_close(pCodecCtx);

	printf("\r\n avformat_close_input!\r\n");
	avformat_close_input(&pFormatCtx);
	
	printf("\r\n end!\r\n");
	return 0;
}
