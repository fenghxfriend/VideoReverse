#include "reverse.h"

#include <android/log.h>
#include <libavutil/timestamp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define FFMPEG_LOG_LEVEL AV_LOG_WARNING
#define LOG_LEVEL 2
#define LOG_TAG "reverse.c"
#define LOGI(level, ...) if (level <= LOG_LEVEL) {__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__);}
#define LOGE(level, ...) if (level <= LOG_LEVEL + 10) {__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__);}
#define LOGW(level, ...) if (level <= LOG_LEVEL + 5) {__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__);}

int open_codec_context(int *stream_idx, AVFormatContext *fmt_ctx, enum AVMediaType type);
int decode_packet(int *got_frame, AVFrame*, int cached, int, int);
void encode_frame_to_dst(AVFrame *frame, int frameCount);

AVFormatContext *fmt_ctx = NULL;
AVFormatContext *fmt_ctx_o = NULL;
AVOutputFormat *fmt_o;
AVStream *video_o;
AVCodec *codec_o;
uint8_t *picture_buf_o;
AVFrame *picture_o;
AVPacket pkt_o;


AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx, *video_enc_ctx = NULL;
AVStream *video_stream = NULL, *audio_stream = NULL;
AVPacket pkt;
int video_frame_count = 0;
int audio_frame_count = 0;
FILE *video_dst_file = NULL;
FILE *audio_dst_file = NULL;
int video_stream_idx = -1, audio_stream_idx = -1;
uint8_t *video_dst_data[4] = {NULL};
int      video_dst_linesize[4];
int      video_dst_bufsize;

uint8_t **audio_dst_data = NULL;
int       audio_dst_linesize;
int       audio_dst_bufsize;

int ret = 0, got_frame;

int reverse(char *file_path_src, char *file_path_desc,
            long positionUsStart, long positionUsEnd,
            int video_stream_no, int audio_stream_no,
            int subtitle_stream_no) {
  
  char *src_filename = NULL;
  char *video_dst_filename = NULL;
  char *audio_dst_filename = NULL;

  src_filename = file_path_src;
  video_dst_filename = file_path_desc;
  //audio_dst_filename = argv[3];

  /* register all formats and codecs */
  av_register_all();

  /* open input file, and allocated format context */
  LOGI(LOG_LEVEL, "source file %s\n", src_filename);
  LOGI(LOG_LEVEL, "destination file %s\n", video_dst_filename);
  if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
      LOGI(LOG_LEVEL, "Could not open source file %s\n", src_filename);
      exit(1);
  }

  /* retrieve stream information */
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
      LOGI(LOG_LEVEL, "Could not find stream information\n");
      exit(1);
  }

  if (video_stream_no > 0 &&
      open_codec_context(&video_stream_idx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
      video_stream = fmt_ctx->streams[video_stream_idx];
      video_dec_ctx = video_stream->codec;
      video_dst_file = fopen(video_dst_filename, "wb");
      if (!video_dst_file) {
          LOGI(LOG_LEVEL, "Could not open video destination file %s(%d)\n", video_dst_filename, errno);
          ret = 1;
          goto end;
      }

      /* allocate image where the decoded image will be put */
      ret = av_image_alloc(video_dst_data, video_dst_linesize,
                           video_dec_ctx->width, video_dec_ctx->height,
                           video_dec_ctx->pix_fmt, 1);
      if (ret < 0) {
          LOGI(LOG_LEVEL, "Could not allocate raw video buffer\n");
          goto end;
      }
      video_dst_bufsize = ret;
  }

  /* dump input information to stderr */
  av_dump_format(fmt_ctx, 0, src_filename, 0);
  if (audio_stream_no > 0 &&
      open_codec_context(&audio_stream_idx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
      int nb_planes;

      audio_stream = fmt_ctx->streams[audio_stream_idx];
      audio_dec_ctx = audio_stream->codec;
      audio_dst_file = fopen(audio_dst_filename, "wb");
      if (!audio_dst_file) {
          LOGI(LOG_LEVEL, "Could not open audio destination file %s(%d)\n", video_dst_filename, errno);
          ret = 1;
          goto end;
      }

      nb_planes = av_sample_fmt_is_planar(audio_dec_ctx->sample_fmt) ?
          audio_dec_ctx->channels : 1;
      audio_dst_data = av_mallocz(sizeof(uint8_t *) * nb_planes);
      if (!audio_dst_data) {
          LOGI(LOG_LEVEL, "Could not allocate audio data buffers\n");
          ret = AVERROR(ENOMEM);
          goto end;
      }
  }
  if (!audio_stream && !video_stream) {
      LOGI(LOG_LEVEL, "Could not find audio or video stream in the input, aborting\n");
      ret = 1;
      goto end;
  }

  AVFrame *frame = avcodec_alloc_frame();
  if (!frame) {
      LOGI(LOG_LEVEL, "Could not allocate frame\n");
      ret = AVERROR(ENOMEM);
      goto end;
  }

  /* initialize packet, set data to NULL, let the demuxer fill it */
  av_init_packet(&pkt);
  pkt.data = NULL;
  pkt.size = 0;
  
  av_init_packet(&pkt_o);
  pkt_o.data = NULL;
  pkt_o.size = 0;

  if (video_stream)
      LOGI(LOG_LEVEL, "Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
  if (audio_stream)
      LOGI(LOG_LEVEL, "Demuxing video from file '%s' into '%s'\n", src_filename, audio_dst_filename);

  codec_o = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec_o) {
    fprintf(stderr, "Codec not found\n");
    exit(1);
  }

  video_enc_ctx = avcodec_alloc_context3(codec_o);
  video_enc_ctx->bit_rate = video_dec_ctx->bit_rate;
  video_enc_ctx->width = video_dec_ctx->width;
  video_enc_ctx->height = video_dec_ctx->height;
  video_enc_ctx->time_base = video_dec_ctx->time_base;
  video_enc_ctx->gop_size = video_dec_ctx->gop_size;
  video_enc_ctx->max_b_frames = video_dec_ctx->max_b_frames;
  video_enc_ctx->pix_fmt = video_dec_ctx->pix_fmt;
  video_enc_ctx->priv_data = video_dec_ctx->priv_data;
  /* open it */
  if (avcodec_open2(video_enc_ctx, codec_o, NULL) < 0) {
    fprintf(stderr, "Could not open codec\n");
    exit(1);
  }

  /* read frames from the file */
  int frameCount = 0;
  while (av_read_frame(fmt_ctx, &pkt) >= 0) {
    // decode pkt to frame
    decode_packet(&got_frame, frame, 0, frameCount, 0);
    if (got_frame) {
      encode_frame_to_dst(frame, frameCount);
    }
    frameCount++;
  }

  /* flush cached frames */
  pkt.data = NULL;
  pkt.size = 0;
  do {
      decode_packet(&got_frame, frame, 1, frameCount, 0);
      if (got_frame) {
      encode_frame_to_dst(frame, frameCount);
    }
    frameCount++;
  } while (got_frame);
  LOGI(LOG_LEVEL, "Demuxing succeeded.\n");


  if (video_stream) {
      LOGI(LOG_LEVEL, "Play the output video file with the command:\n"
             "ffplay -f rawvideo -pix_fmt %s -video_size %d*%d %s\n",
             av_get_pix_fmt_name(video_dec_ctx->pix_fmt),
             video_dec_ctx->width, video_dec_ctx->height,
             video_dst_filename);
  }
  if (audio_stream) {
      const char *fmt;

      if ((ret = get_format_from_sample_fmt(&fmt, audio_dec_ctx->sample_fmt) < 0))
          goto end;
      LOGI(LOG_LEVEL, "Play the output audio file with the command:\n"
             "ffplay -f %s -ac %d -ar %d %s\n",
             fmt, audio_dec_ctx->channels, audio_dec_ctx->sample_rate,
             audio_dst_filename);
  }

end:
  if (video_dec_ctx)
      avcodec_close(video_dec_ctx);
  if (audio_dec_ctx)
      avcodec_close(audio_dec_ctx);
  avformat_close_input(&fmt_ctx);
  if (video_dst_file)
      fclose(video_dst_file);
  if (audio_dst_file)
      fclose(audio_dst_file);
  av_free(frame);
  av_free(video_dst_data[0]);
  av_free(audio_dst_data);

  return ret < 0;
}

void encode_frame_to_dst(AVFrame *frame, int frameCount) {
  /* write to another file */
  av_init_packet(&pkt_o);
  pkt_o.data = NULL;    // packet data will be allocated by the encoder
  pkt_o.size = 0;
  fflush(stdout);

  int x = 0, y = 0;
  
  /* prepare a dummy image */
  /* Y */
  for(y=0;y<video_enc_ctx->height;y++) {
      for(x=0;x<video_enc_ctx->width;x++) {
          frame->data[0][y * frame->linesize[0] + x] = x + y + frameCount * 3;
      }
  }

  /* Cb and Cr */
  x = y = 0;
  for(y=0;y<video_enc_ctx->height/2;y++) {
      for(x=0;x<video_enc_ctx->width/2;x++) {
          frame->data[1][y * frame->linesize[1] + x] = 128 + y + frameCount * 2;
          frame->data[2][y * frame->linesize[2] + x] = 64 + x + frameCount * 5;
      }
  }

  frame->pts = frameCount;
  /* encode the image */
  int got_output;
  ret = avcodec_encode_video2(video_enc_ctx, &pkt_o, frame, &got_output);
  if (ret < 0) {
      fprintf(stderr, "Error encoding frame\n");
      exit(1);
  }

  if (got_output) {
      printf("Write frame %3d (size=%5d)\n", frameCount, pkt_o.size);
      fwrite(pkt_o.data, 1, pkt_o.size, video_dst_file);
      av_free_packet(&pkt);
  }
}

int open_codec_context(int *stream_idx, AVFormatContext *fmt_ctx, enum AVMediaType type) {
  int ret;
  AVStream *st;
  AVCodecContext *dec_ctx = NULL;
  AVCodec *dec = NULL;

  ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
  if (ret < 0) {
    LOGI(LOG_LEVEL, "Could not find %s stream in inout file '%s'\n",
         av_get_media_type_string(type), "src_filename");
    return ret;
  } else {
    *stream_idx = ret;
    st = fmt_ctx->streams[ret];

    /* find decoder for the stream */
    dec_ctx = st->codec;
    dec = avcodec_find_decoder(dec_ctx->codec_id);
    if (!dec) {
      LOGI(LOG_LEVEL, "Failed to find %s codec\n", av_get_media_type_string(type));
      return ret;
    }
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
      LOGI(LOG_LEVEL, "Failed to open %s codec\n", av_get_media_type_string(type));
      return ret;
    }
    return 0;
  }
}

int decode_packet(int *got_frame, AVFrame *frame, 
                  int cached, int frameCount,
                  int writePictureToFile) {
  int ret = 0;
  if (pkt.stream_index == video_stream_idx) {
    ret = avcodec_decode_video2(video_dec_ctx, frame, got_frame, &pkt);
    if (ret < 0) {
      LOGI(LOG_LEVEL, "Error decoding video frame\n");
      return ret;
    }
    if (*got_frame) {
      LOGI(LOG_LEVEL, "video_frame%s n:%d coded_n:%d pts:%s\n",
           cached ? "(cached)" : "",
           video_frame_count++, frame->coded_picture_number,
           av_ts2timestr(frame->pts, &video_dec_ctx->time_base));

      if (writePictureToFile) {
        /* copy decoded frame to dest buffer */
        av_image_copy(video_dst_data, video_dst_linesize,
                      (const uint8_t **)(frame->data),
                      frame->linesize,
                      video_dec_ctx->pix_fmt,
                      video_dec_ctx->width,
                      video_dec_ctx->height);
        /* write to rawvideo file */
        fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
      }
    }
  } else if (pkt.stream_index == audio_stream_idx) {
    // TODO
  }
}

int get_format_from_sample_fmt(const char **fmt, enum AVSampleFormat sample_fmt) {
  int i;
  struct sample_fmt_entry {
    enum AVSampleFormat sample_fmt;
    const char *fmt_be, *fmt_le;
  } sample_fmt_entries[] = {
    { AV_SAMPLE_FMT_U8,  "u8",     "u8"    },
    { AV_SAMPLE_FMT_S16, "s16be",  "s16le" },
    { AV_SAMPLE_FMT_S32, "s32be",  "s32le" },
    { AV_SAMPLE_FMT_FLT, "f32be",  "f32le" },
    { AV_SAMPLE_FMT_DBL, "f64be",  "f64le" },
  };
  *fmt = NULL;

  for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
    struct sample_fmt_entry *entry = &sample_fmt_entries[i];
    if (sample_fmt == entry->sample_fmt) {
      *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
      return 0;
    }
  }

  LOGI(LOG_LEVEL, "sample format %s is not supported as output format\n",
       av_get_sample_fmt_name(sample_fmt));
  return -1;
}

