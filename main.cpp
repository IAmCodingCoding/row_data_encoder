#include <iostream>
#include <thread>
#include <sys/time.h>
#include <mutex>

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libavutil/audio_fifo.h"
}

AVPixelFormat dst_pix_format = AV_PIX_FMT_YUV420P;
int dst_width = 640;
int dst_height = 480;
int frame_rate = 30;
int yuv_data_length = dst_width * dst_height * 1.5;

AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;
int dst_sample_rate = 44100;
int dst_channels = 2;
int dst_nb_samples = 1024;
int pcm_data_length = 7168;

AVFormatContext *out_format = nullptr;
int video_index = 0;
int audio_index = 0;
std::mutex write_lock;

AVCodecContext *videoEncoder = nullptr;
AVCodecContext *audioEncoder = nullptr;

AVAudioFifo *fifo_buffer = nullptr;

std::mutex end_lock;
bool isEnd = false;

timeval t;
long startTime = 0;
std::mutex time_lock;

long getTime() {
    std::unique_lock<std::mutex> lock(time_lock);
    gettimeofday(&t, nullptr);
    if (startTime == 0) {
        startTime = t.tv_sec * 1000 + t.tv_usec / 1000;
        return 0L;
    } else {
        return t.tv_sec * 1000 + t.tv_usec / 1000 - startTime;
    }
}

int init(const char *url);


void end();

int main() {
    const char *url = "./output.mp4";
    int ret = init(url);
    if (ret < 0) {
        return ret;
    }
    std::thread video_thread([&]() {
        FILE *video_fd = fopen("../test_data/test.yuv", "r");
        uint8_t *buffer = (uint8_t *) malloc(yuv_data_length);
        fread(buffer, 1, yuv_data_length, video_fd);
//       data[0]= 0--->w*h;
//        data[1]=w*h---->w*h+w*h/4;
//        data[2]=w*h+w*h/4--->end;
        const uint8_t *yuv[4] = {buffer, buffer + dst_width * dst_height,
                                 buffer + dst_width * dst_height + dst_width * dst_height / 4};

        const int line_size[4] = {dst_width, dst_width / 2, dst_width / 2};
        AVFrame *frame = av_frame_alloc();
        av_image_alloc(frame->data, frame->linesize, dst_width, dst_height, dst_pix_format, 1);
        av_image_copy(frame->data, frame->linesize, yuv, line_size, dst_pix_format, dst_width, dst_height);
        frame->width = dst_width;
        frame->height = dst_height;
        frame->format = dst_pix_format;
        AVPacket *video_packet = av_packet_alloc();
        while (1) {
            long time = getTime();
            avcodec_send_frame(videoEncoder, frame);
            while (avcodec_receive_packet(videoEncoder, video_packet) >= 0) {
                video_packet->pts = (time / 1000.0) / av_q2d(out_format->streams[video_index]->time_base);
                video_packet->stream_index = video_index;
                std::unique_lock<std::mutex> lock(write_lock);
                av_interleaved_write_frame(out_format, video_packet);
            }
            if (time > 4000) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        //frame经过了av_image_alloc处理需要单独清理frame.data[0]
        av_freep(&frame->data[0]);
        av_frame_free(&frame);
        av_packet_free(&video_packet);
        free(buffer);
        fclose(video_fd);
    });
    std::thread audio_thread([&]() {
        FILE *audio_fd = fopen("../test_data/test.pcm", "r");
        uint8_t *buffer = (uint8_t *) malloc(pcm_data_length);
        fread(buffer, 1, pcm_data_length, audio_fd);
        AVFrame *frame = av_frame_alloc();
        frame->sample_rate = dst_sample_rate;
        frame->format = dst_sample_fmt;
        frame->channels = dst_channels;
        frame->channel_layout = av_get_default_channel_layout(frame->channels);
        av_samples_alloc(frame->data, frame->linesize, frame->channels, audioEncoder->frame_size,
                         (AVSampleFormat) frame->format, 1);
        AVPacket *audio_packet = av_packet_alloc();
        while (1) {
            void *data[] = {buffer};
            int nb_samples = (pcm_data_length / dst_channels) / av_get_bytes_per_sample(dst_sample_fmt);
            long time = getTime();
            time -= (long) ((av_audio_fifo_size(fifo_buffer) / (double) dst_sample_rate) * 1000);
            av_audio_fifo_write(fifo_buffer, data, nb_samples);
            while (av_audio_fifo_size(fifo_buffer) >= audioEncoder->frame_size) {
                int read_samples = av_audio_fifo_read(fifo_buffer, (void **) frame->data, audioEncoder->frame_size);
                frame->nb_samples = read_samples;
                avcodec_send_frame(audioEncoder, frame);
                int count = 0;
                while (avcodec_receive_packet(audioEncoder, audio_packet) >= 0) {
                    int real_time = time + (count++) * audioEncoder->frame_size / dst_sample_rate * 1000;
                    audio_packet->pts = (int64_t) ((real_time / 1000.0) / av_q2d(audioEncoder->time_base));
                    audio_packet->stream_index = audio_index;
                    std::unique_lock<std::mutex> lock(write_lock);
                    av_interleaved_write_frame(out_format, audio_packet);
                }
                time += (read_samples / (double) dst_sample_rate) * 1000;
            }
            if (time > 4000) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        //frame经过了av_samples_alloc处理需要单独清理frame.data[0]
        av_freep(&frame->data[0]);
        av_frame_free(&frame);
        av_packet_free(&audio_packet);
        free(buffer);
        fclose(audio_fd);
    });

    video_thread.join();
    audio_thread.join();
    end();
    return 0;
}

void end() {
    av_write_trailer(out_format);
    if (!(out_format->flags & AVFMT_NOFILE)) {
        avio_close(out_format->pb);
    }
    avformat_free_context(out_format);
    avcodec_free_context(&audioEncoder);
    avcodec_free_context(&videoEncoder);
    av_audio_fifo_free(fifo_buffer);
}

int init(const char *url) {
    int ret = avformat_alloc_output_context2(&out_format, nullptr, nullptr, url);
    if (ret < 0) {
        printf("fail to alloc output context\n");
        return ret;
    }
    if (!(out_format->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&out_format->pb, url, AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) {
            printf("fail to open io context\n");
        }
    }
    AVCodec *video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (video_codec == nullptr) {
        printf("not supported encoder:%d\n", AV_CODEC_ID_H264);
        return -1;
    }

    videoEncoder = avcodec_alloc_context3(video_codec);
    videoEncoder->pix_fmt = dst_pix_format;
    videoEncoder->width = dst_width;
    videoEncoder->height = dst_height;
    videoEncoder->time_base = {1, frame_rate};
    videoEncoder->max_b_frames = 0;
    videoEncoder->bit_rate = (int) (1024 * 1024 * 0.5);
    videoEncoder->gop_size = 15;
    if (out_format->oformat->flags & AVFMT_GLOBALHEADER) {
        videoEncoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary *p = nullptr;
    av_dict_set(&p, "preset", "fast", 0);
    av_dict_set(&p, "tune", "zerolatency", 0);
    ret = avcodec_open2(videoEncoder, videoEncoder->codec, &p);
    if (ret < 0) {
        printf("fail to open encoder:%s\n", video_codec->long_name);
        return ret;
    }

    AVStream *video_stream = avformat_new_stream(out_format, nullptr);
    ret = avcodec_parameters_from_context(video_stream->codecpar, videoEncoder);
    if (ret < 0) {
        printf("fail to fill paramters from video encoder\n");
        return ret;
    }
    video_index = video_stream->index;

    AVCodec *audio_codec = avcodec_find_encoder_by_name("libfdk_aac");
    if (audio_codec == nullptr) {
        printf("not supported encoder:libfdk_aac\n");
        return -1;
    }
    audioEncoder = avcodec_alloc_context3(audio_codec);
    audioEncoder->sample_fmt = AV_SAMPLE_FMT_S16;
    audioEncoder->time_base = {1, dst_sample_rate};
    audioEncoder->bit_rate = 64000;
    audioEncoder->sample_rate = dst_sample_rate;
    audioEncoder->channels = dst_channels;
    audioEncoder->channel_layout = av_get_default_channel_layout(dst_channels);
    if (out_format->oformat->flags & AVFMT_GLOBALHEADER) {
        audioEncoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ret = avcodec_open2(audioEncoder, audio_codec, nullptr);
    if (ret < 0) {
        printf("fail to open encoder:%s\n", audio_codec->long_name);
        return ret;
    }
    AVStream *audio_stream = avformat_new_stream(out_format, nullptr);
    ret = avcodec_parameters_from_context(audio_stream->codecpar, audioEncoder);
    if (ret < 0) {
        printf("fail to fill paramters from audio encoder\n");
        return ret;
    }
    audio_index = audio_stream->index;


    fifo_buffer = av_audio_fifo_alloc(dst_sample_fmt, dst_channels, audioEncoder->frame_size);
    ret = avformat_write_header(out_format, nullptr);
    if (ret < 0) {
        printf("fail to write header\n");
    }
    return ret;
}
