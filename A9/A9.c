#include <gtk/gtk.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>

#define BUFFER_SIZE 8192
#define BUFFER_COUNT 8
#define MAX_SAMPLES 2048
#define NUM_BARS 48

typedef struct
{
    float data[BUFFER_SIZE];
    int size;
    int read_pos;
    int full;
} MyAudioBuffer;

MyAudioBuffer circular_buffer[BUFFER_COUNT];
int read_index = 0, write_index = 0;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

AVFormatContext *fmt_ctx = NULL;
AVCodecContext *codec_ctx = NULL;
SwrContext *swr_ctx = NULL;
int audio_stream_index = -1;

AudioUnit audio_unit;

GtkWidget *drawing_area;
int theme_index = 0;
int visual_mode = 2; // 2 = bars (default)
const char *themes[] = {"#00FFFF", "#FF00FF", "#00FF00", "#FF6600"};
float last_frame[MAX_SAMPLES] = {0};
float smooth_bars[NUM_BARS] = {0};
int last_frame_len = 0;

void *decoder_thread(void *arg)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (av_read_frame(fmt_ctx, packet) >= 0)
    {
        if (packet->stream_index == audio_stream_index)
        {
            avcodec_send_packet(codec_ctx, packet);
            while (avcodec_receive_frame(codec_ctx, frame) == 0)
            {
                float *converted_data = NULL;
                int dst_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, codec_ctx->sample_rate) + frame->nb_samples,
                    codec_ctx->sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);

                av_samples_alloc((uint8_t **)&converted_data, NULL, 1, dst_nb_samples, AV_SAMPLE_FMT_FLT, 0);
                int samples = swr_convert(swr_ctx, (uint8_t **)&converted_data, dst_nb_samples,
                                          (const uint8_t **)frame->data, frame->nb_samples);

                pthread_mutex_lock(&buffer_mutex);
                int copied = 0;
                while (copied < samples)
                {
                    while (circular_buffer[write_index].full)
                    {
                        pthread_cond_wait(&buffer_not_full, &buffer_mutex);
                    }
                    int chunk = samples - copied;
                    if (chunk > BUFFER_SIZE)
                        chunk = BUFFER_SIZE;

                    memcpy(circular_buffer[write_index].data, &converted_data[copied], chunk * sizeof(float));
                    circular_buffer[write_index].size = chunk;
                    circular_buffer[write_index].read_pos = 0;
                    circular_buffer[write_index].full = 1;

                    write_index = (write_index + 1) % BUFFER_COUNT;
                    pthread_cond_signal(&buffer_not_empty);
                    copied += chunk;
                }
                pthread_mutex_unlock(&buffer_mutex);

                av_freep(&converted_data);
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    return NULL;
}

OSStatus render_callback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
                         const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                         UInt32 inNumberFrames, AudioBufferList *ioData)
{
    pthread_mutex_lock(&buffer_mutex);
    while (!circular_buffer[read_index].full)
    {
        pthread_cond_wait(&buffer_not_empty, &buffer_mutex);
    }

    float *buffer = (float *)ioData->mBuffers[0].mData;
    int frames = inNumberFrames;
    int copied = 0;

    while (copied < frames)
    {
        int available = circular_buffer[read_index].size - circular_buffer[read_index].read_pos;
        int chunk = (frames - copied < available) ? (frames - copied) : available;
        memcpy(&buffer[copied], &circular_buffer[read_index].data[circular_buffer[read_index].read_pos], chunk * sizeof(float));
        circular_buffer[read_index].read_pos += chunk;
        copied += chunk;

        if (circular_buffer[read_index].read_pos >= circular_buffer[read_index].size)
        {
            circular_buffer[read_index].full = 0;
            read_index = (read_index + 1) % BUFFER_COUNT;
            pthread_cond_signal(&buffer_not_full);
        }
    }

    pthread_mutex_unlock(&buffer_mutex);
    return noErr;
}

void draw_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    cairo_set_source_rgb(cr, 1, 1, 1); // white background
    cairo_paint(cr);

    GdkRGBA color;
    gdk_rgba_parse(&color, themes[theme_index]);
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1.0);
    cairo_set_line_width(cr, 2);

    pthread_mutex_lock(&buffer_mutex);
    if (circular_buffer[read_index].full)
    {
        int n = circular_buffer[read_index].size;
        int step = n / width;
        if (step < 1)
            step = 1;
        for (int i = 0; i < width && i * step < n; i++)
        {
            last_frame[i] = circular_buffer[read_index].data[i * step];
        }
        last_frame_len = width;
    }
    pthread_mutex_unlock(&buffer_mutex);

    if (visual_mode == 1)
    {
        cairo_move_to(cr, 0, height / 2);
        for (int i = 0; i < last_frame_len; i++)
        {
            float y = height / 2 - last_frame[i] * (height / 2);
            cairo_line_to(cr, i, y);
        }
        cairo_stroke(cr);
    }
    else if (visual_mode == 2)
    {
        int bars = NUM_BARS;
        int samples_per_bar = last_frame_len / bars;
        if (samples_per_bar < 1)
            samples_per_bar = 1;

        for (int i = 0; i < bars; i++)
        {
            float sum = 0;
            for (int j = 0; j < samples_per_bar; j++)
            {
                int idx = i * samples_per_bar + j;
                if (idx < last_frame_len)
                    sum += fabsf(last_frame[idx]);
            }

            float avg = sum / samples_per_bar;

            // Smooth animation: exponential moving average
            smooth_bars[i] = 0.8 * smooth_bars[i] + 0.2 * avg;

            float bar_height = smooth_bars[i] * height;
            float x = (i * width) / bars;
            float w = width / bars - 2;
            if (w < 2)
                w = 2;

            cairo_rectangle(cr, x, height - bar_height, w, bar_height);
        }

        cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1.0);
        cairo_fill(cr);
    }
}

gboolean refresh(gpointer data)
{
    if (!GTK_IS_WIDGET(data))
        return G_SOURCE_CONTINUE;
    gtk_widget_queue_draw(GTK_WIDGET(data));
    return G_SOURCE_CONTINUE;
}

gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType mods, gpointer user_data)
{
    if (keyval == GDK_KEY_c || keyval == GDK_KEY_C)
        theme_index = (theme_index + 1) % (sizeof(themes) / sizeof(themes[0]));
    else if (keyval == GDK_KEY_1)
        visual_mode = 1;
    else if (keyval == GDK_KEY_2)
        visual_mode = 2;
    return FALSE;
}

void init_coreaudio()
{
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple};

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioComponentInstanceNew(comp, &audio_unit);

    AURenderCallbackStruct callback = {.inputProc = render_callback};
    AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Global, 0, &callback, sizeof(callback));

    AudioStreamBasicDescription format = {0};
    format.mSampleRate = codec_ctx->sample_rate; // use actual sample rate
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = 4;
    format.mBytesPerPacket = 4;

    AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &format, sizeof(format));

    AudioUnitInitialize(audio_unit);
    AudioOutputUnitStart(audio_unit);
}

void on_activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Audio Visualizer");
    gtk_window_set_default_size(GTK_WINDOW(win), 550, 400);

    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_focusable(drawing_area, TRUE); // ✅ Make focusable
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_waveform, NULL, NULL);
    gtk_window_set_child(GTK_WINDOW(win), drawing_area);

    GtkEventController *controller = gtk_event_controller_key_new();
    gtk_widget_add_controller(drawing_area, controller); // ✅ Attach controller
    g_signal_connect(controller, "key-pressed", G_CALLBACK(key_pressed), NULL);

    gtk_widget_grab_focus(drawing_area); // ✅ Grab focus

    g_timeout_add(16, refresh, GTK_WIDGET(drawing_area));
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char *argv[])
{
    const char *filename = "input.mp3"; // hardcoded audio file

    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    avformat_open_input(&fmt_ctx, filename, NULL, NULL);
    avformat_find_stream_info(fmt_ctx, NULL);

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++)
    {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_index = i;
            break;
        }
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    avcodec_open2(codec_ctx, codec, NULL);

    AVChannelLayout in_layout, out_layout;
    av_channel_layout_copy(&in_layout, &codecpar->ch_layout);
    av_channel_layout_default(&out_layout, 1); // mono

    swr_alloc_set_opts2(&swr_ctx,
                        &out_layout, AV_SAMPLE_FMT_FLT, codec_ctx->sample_rate,
                        &in_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
                        0, NULL);
    swr_init(swr_ctx);

    init_coreaudio();

    pthread_t thread;
    pthread_create(&thread, NULL, decoder_thread, NULL);

    GtkApplication *app = gtk_application_new("org.visualizer.demo", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);

    AudioOutputUnitStop(audio_unit);
    AudioComponentInstanceDispose(audio_unit);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return status;
}