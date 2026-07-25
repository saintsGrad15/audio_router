#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kMaxDevices 64
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    AudioDeviceID device_id;
    char name[128];
    int has_input;
    int has_output;
} DeviceInfo;

static volatile int g_running = 1;
static AudioUnit g_au = NULL;
static AudioUnit g_input_au = NULL;
static AudioUnit g_output_au = NULL;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_output_au) AudioOutputUnitStop(g_output_au);
    if (g_input_au) AudioOutputUnitStop(g_input_au);
    if (g_au) AudioOutputUnitStop(g_au);
}

static OSStatus get_audio_devices(DeviceInfo *devices, int *count) {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &prop, 0, NULL, &size);
    if (err != noErr) return err;

    int num_devices = (int)(size / sizeof(AudioDeviceID));
    if (num_devices > kMaxDevices) num_devices = kMaxDevices;

    AudioDeviceID device_ids[kMaxDevices];
    err = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &prop, 0, NULL, &size, device_ids);
    if (err != noErr) return err;

    int found = 0;
    for (int i = 0; i < num_devices && found < kMaxDevices; i++) {
        AudioObjectPropertyAddress name_prop = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        CFStringRef name_ref = NULL;
        UInt32 name_size = sizeof(name_ref);
        err = AudioObjectGetPropertyData(
            device_ids[i], &name_prop, 0, NULL, &name_size, &name_ref);
        if (err != noErr || !name_ref) continue;

        CFStringGetCString(name_ref, devices[found].name,
                          sizeof(devices[found].name),
                          kCFStringEncodingUTF8);
        CFRelease(name_ref);

        AudioObjectPropertyAddress ch_prop = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };

        UInt32 ch_size = 0;
        AudioObjectGetPropertyDataSize(device_ids[i], &ch_prop,
                                       0, NULL, &ch_size);
        AudioBufferList *buf_list = (AudioBufferList *)malloc(ch_size);
        if (!buf_list) continue;

        AudioObjectGetPropertyData(device_ids[i], &ch_prop,
                                   0, NULL, &ch_size, buf_list);

        int in_channels = 0;
        for (UInt32 b = 0; b < buf_list->mNumberBuffers; b++)
            in_channels += buf_list->mBuffers[b].mNumberChannels;
        free(buf_list);

        ch_prop.mScope = kAudioDevicePropertyScopeOutput;
        AudioObjectGetPropertyDataSize(device_ids[i], &ch_prop,
                                       0, NULL, &ch_size);
        buf_list = (AudioBufferList *)malloc(ch_size);
        if (!buf_list) continue;

        AudioObjectGetPropertyData(device_ids[i], &ch_prop,
                                   0, NULL, &ch_size, buf_list);

        int out_channels = 0;
        for (UInt32 b = 0; b < buf_list->mNumberBuffers; b++)
            out_channels += buf_list->mBuffers[b].mNumberChannels;
        free(buf_list);

        if (in_channels > 0 || out_channels > 0) {
            devices[found].device_id = device_ids[i];
            devices[found].has_input = (in_channels > 0);
            devices[found].has_output = (out_channels > 0);
            found++;
        }
    }
    *count = found;
    return noErr;
}

static void list_devices(void) {
    DeviceInfo devices[kMaxDevices];
    int count = 0;
    get_audio_devices(devices, &count);

    printf("%-4s %-45s %-8s %-8s\n", "ID", "Name", "Input", "Output");
    printf("%-4s %-45s %-8s %-8s\n", "------",
           "---------------------------------------------", "------", "------");
    for (int i = 0; i < count; i++) {
        printf("%-4d %-45s %-8s %-8s\n",
               i, devices[i].name,
               devices[i].has_input ? "Yes" : "-",
               devices[i].has_output ? "Yes" : "-");
    }
}

static AudioDeviceID resolve_device(const char *arg, int require_input) {
    DeviceInfo devices[kMaxDevices];
    int count = 0;
    get_audio_devices(devices, &count);

    int is_numeric = 1;
    for (const char *p = arg; *p; p++) {
        if (*p < '0' || *p > '9') { is_numeric = 0; break; }
    }

    if (is_numeric) {
        int idx = atoi(arg);
        if (idx >= 0 && idx < count) {
            if (require_input && !devices[idx].has_input) {
                fprintf(stderr, "Device %d has no input channels\n", idx);
                exit(1);
            }
            if (!require_input && !devices[idx].has_output) {
                fprintf(stderr, "Device %d has no output channels\n", idx);
                exit(1);
            }
            return devices[idx].device_id;
        }
    }

    for (int i = 0; i < count; i++) {
        if (strstr(devices[i].name, arg) != NULL) {
            if (require_input && !devices[i].has_input) continue;
            if (!require_input && !devices[i].has_output) continue;
            return devices[i].device_id;
        }
    }

    fprintf(stderr, "Device not found: %s\n", arg);
    exit(1);
}

static AudioDeviceID get_default_device(int input) {
    AudioObjectPropertyAddress prop = {
        input ? kAudioHardwarePropertyDefaultInputDevice
              : kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioDeviceID dev = kAudioDeviceUnknown;
    UInt32 size = sizeof(dev);
    OSStatus err = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &prop, 0, NULL, &size, &dev);
    if (err != noErr || dev == kAudioDeviceUnknown) {
        fprintf(stderr, "No default %s device found\n",
                input ? "input" : "output");
        exit(1);
    }
    return dev;
}

static void get_device_name(AudioDeviceID dev, char *buf, size_t len) {
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef ref = NULL;
    UInt32 size = sizeof(ref);
    OSStatus err = AudioObjectGetPropertyData(dev, &prop, 0, NULL, &size, &ref);
    if (err == noErr && ref) {
        CFStringGetCString(ref, buf, (CFIndex)len, kCFStringEncodingUTF8);
        CFRelease(ref);
    } else {
        snprintf(buf, len, "Device %d", (int)dev);
    }
}

static Float64 get_device_sample_rate(AudioDeviceID dev) {
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(dev, &prop, 0, NULL, &size, &rate);
    return rate;
}

static void print_format(const char *label, const AudioStreamBasicDescription *f) {
    const char *type = "unknown";
    if (f->mFormatID == kAudioFormatLinearPCM) {
        if (f->mFormatFlags & kAudioFormatFlagIsFloat)
            type = "float";
        else if (f->mFormatFlags & kAudioFormatFlagIsSignedInteger)
            type = "int";
    }
    int interleaved = !(f->mFormatFlags & kAudioFormatFlagIsNonInterleaved);
    printf("  %s: %s %s %u-bit, %u ch, %.0f Hz\n",
           label, type, interleaved ? "interleaved" : "non-interleaved",
           (unsigned)f->mBitsPerChannel, (unsigned)f->mChannelsPerFrame,
           f->mSampleRate);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n\n"
        "Routes audio input to output with low latency on macOS.\n\n"
        "Options:\n"
        "  -l              List audio devices\n"
        "  -t              Play 440 Hz test tone (output only)\n"
        "  -m              Meter input levels (input only)\n"
        "  -i <device>     Input device (index or name substring)\n"
        "  -o <device>     Output device (index or name substring)\n"
        "  -b <frames>     Buffer size in frames (default: 256)\n"
        "  -h              Show this help\n\n"
        "Examples:\n"
        "  %s -l                List devices\n"
        "  %s                   Route default input to default output\n"
        "  %s -t                Play test tone on default output\n"
        "  %s -m -i 7           Meter input levels from device 7 (GX-10)\n"
        "  %s -i 2 -o 4         Route device 2 input to device 4 output\n"
        "  %s -b 128            Use smaller buffer (~2.7ms at 48kHz)\n",
        prog, prog, prog, prog, prog, prog, prog);
}

typedef struct {
    AudioBufferList *input_buf;
    UInt32 input_channels;
    int input_interleaved;
    int test_mode;
    double phase;
    volatile float peak_l;
    volatile float peak_r;
} RenderContext;

static OSStatus same_device_input_callback(
    void *in_ref_con,
    AudioUnitRenderActionFlags *io_action_flags,
    const AudioTimeStamp *in_time_stamp,
    UInt32 in_bus_number,
    UInt32 in_number_frames,
    AudioBufferList *io_data)
{
    (void)io_data;

    RenderContext *ctx = (RenderContext *)in_ref_con;

    OSStatus err = AudioUnitRender(g_au, io_action_flags, in_time_stamp,
                                   1, in_number_frames, ctx->input_buf);
    return err;
}

static OSStatus route_input_callback(
    void *in_ref_con,
    AudioUnitRenderActionFlags *io_action_flags,
    const AudioTimeStamp *in_time_stamp,
    UInt32 in_bus_number,
    UInt32 in_number_frames,
    AudioBufferList *io_data)
{
    (void)io_data;

    RenderContext *ctx = (RenderContext *)in_ref_con;

    OSStatus err = AudioUnitRender(g_input_au, io_action_flags, in_time_stamp,
                                   1, in_number_frames, ctx->input_buf);
    return err;
}

static OSStatus render_callback(
    void *in_ref_con,
    AudioUnitRenderActionFlags *io_action_flags,
    const AudioTimeStamp *in_time_stamp,
    UInt32 in_bus_number,
    UInt32 in_number_frames,
    AudioBufferList *io_data)
{
    (void)io_action_flags;
    (void)in_time_stamp;
    (void)in_bus_number;

    RenderContext *ctx = (RenderContext *)in_ref_con;

    if (ctx->test_mode) {
        double freq = 440.0;
        double sr = 44100.0;
        double amplitude = 0.3;
        for (UInt32 buf = 0; buf < io_data->mNumberBuffers; buf++) {
            float *data = (float *)io_data->mBuffers[buf].mData;
            UInt32 ch = io_data->mBuffers[buf].mNumberChannels;
            if (ch == 0) ch = 1;
            for (UInt32 s = 0; s < in_number_frames; s++) {
                float sample = (float)(amplitude * sin(ctx->phase));
                for (UInt32 c = 0; c < ch; c++)
                    data[s * ch + c] = sample;
                ctx->phase += 2.0 * M_PI * freq / sr;
            }
        }
        return noErr;
    }

    UInt32 input_bufs = ctx->input_buf->mNumberBuffers;
    UInt32 output_bufs = io_data->mNumberBuffers;

    if (ctx->input_interleaved && input_bufs == 1 && output_bufs > 1) {
        for (UInt32 ch = 0; ch < output_bufs && ch < ctx->input_channels; ch++) {
            UInt32 bytes = io_data->mBuffers[ch].mDataByteSize;
            if (bytes > in_number_frames * sizeof(float))
                bytes = in_number_frames * sizeof(float);
            float *src = (float *)ctx->input_buf->mBuffers[0].mData + ch;
            float *dst = (float *)io_data->mBuffers[ch].mData;
            for (UInt32 s = 0; s < bytes / sizeof(float); s++)
                dst[s] = src[s * ctx->input_channels];
        }
    } else {
        UInt32 bufs = input_bufs < output_bufs ? input_bufs : output_bufs;
        for (UInt32 b = 0; b < bufs; b++) {
            UInt32 bytes = io_data->mBuffers[b].mDataByteSize;
            if (bytes > ctx->input_buf->mBuffers[b].mDataByteSize)
                bytes = ctx->input_buf->mBuffers[b].mDataByteSize;
            memcpy(io_data->mBuffers[b].mData,
                   ctx->input_buf->mBuffers[b].mData, bytes);
        }
    }

    return noErr;
}

static volatile float g_meter_peak_l = 0.0f;
static volatile float g_meter_peak_r = 0.0f;
static AudioBufferList *g_meter_input_buf = NULL;

static OSStatus meter_input_callback(
    void *in_ref_con,
    AudioUnitRenderActionFlags *io_action_flags,
    const AudioTimeStamp *in_time_stamp,
    UInt32 in_bus_number,
    UInt32 in_number_frames,
    AudioBufferList *io_data)
{
    (void)in_ref_con;
    (void)io_data;

    OSStatus err = AudioUnitRender(g_input_au, io_action_flags, in_time_stamp,
                                   1, in_number_frames, g_meter_input_buf);
    if (err != noErr) return err;

    float peak_l = 0.0f, peak_r = 0.0f;
    UInt32 input_bufs = g_meter_input_buf->mNumberBuffers;
    int interleaved = (input_bufs == 1 && g_meter_input_buf->mBuffers[0].mNumberChannels > 1);

    if (interleaved) {
        UInt32 ch = g_meter_input_buf->mBuffers[0].mNumberChannels;
        float *data = (float *)g_meter_input_buf->mBuffers[0].mData;
        for (UInt32 s = 0; s < in_number_frames; s++) {
            float v0 = fabsf(data[s * ch]);
            if (v0 > peak_l) peak_l = v0;
            if (ch > 1) {
                float v1 = fabsf(data[s * ch + 1]);
                if (v1 > peak_r) peak_r = v1;
            }
        }
    } else {
        if (input_bufs > 0) {
            float *data = (float *)g_meter_input_buf->mBuffers[0].mData;
            for (UInt32 s = 0; s < in_number_frames; s++) {
                float v = fabsf(data[s]);
                if (v > peak_l) peak_l = v;
            }
        }
        if (input_bufs > 1) {
            float *data = (float *)g_meter_input_buf->mBuffers[1].mData;
            for (UInt32 s = 0; s < in_number_frames; s++) {
                float v = fabsf(data[s]);
                if (v > peak_r) peak_r = v;
            }
        }
    }

    g_meter_peak_l = peak_l;
    g_meter_peak_r = peak_r;

    return noErr;
}

static OSStatus silent_render(
    void *in_ref_con,
    AudioUnitRenderActionFlags *io_action_flags,
    const AudioTimeStamp *in_time_stamp,
    UInt32 in_bus_number,
    UInt32 in_number_frames,
    AudioBufferList *io_data)
{
    (void)in_ref_con;
    (void)io_action_flags;
    (void)in_time_stamp;
    (void)in_bus_number;
    (void)in_number_frames;

    for (UInt32 b = 0; b < io_data->mNumberBuffers; b++)
        memset(io_data->mBuffers[b].mData, 0, io_data->mBuffers[b].mDataByteSize);

    return noErr;
}

static int setup_meter(AudioDeviceID input_dev, AudioDeviceID output_dev,
                       UInt32 buffer_frames) {
    OSStatus err;

    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);

    err = AudioComponentInstanceNew(comp, &g_input_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to create input AudioUnit: %d\n", (int)err);
        return 1;
    }

    UInt32 enable = 1;
    err = AudioUnitSetProperty(g_input_au, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &enable, sizeof(enable));
    if (err != noErr) {
        fprintf(stderr, "Failed to enable input IO: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitSetProperty(g_input_au, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &input_dev, sizeof(input_dev));
    if (err != noErr) {
        fprintf(stderr, "Failed to set input device: %d\n", (int)err);
        return 1;
    }

    comp = AudioComponentFindNext(NULL, &desc);

    err = AudioComponentInstanceNew(comp, &g_output_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to create output AudioUnit: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &enable, sizeof(enable));
    if (err != noErr) {
        fprintf(stderr, "Failed to enable output IO: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &output_dev, sizeof(output_dev));
    if (err != noErr) {
        fprintf(stderr, "Failed to set output device: %d\n", (int)err);
        return 1;
    }

    AudioStreamBasicDescription input_format;
    UInt32 fmt_size = sizeof(input_format);
    err = AudioUnitGetProperty(g_input_au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 1,
                               &input_format, &fmt_size);
    if (err != noErr) {
        fprintf(stderr, "Failed to get input format: %d\n", (int)err);
        return 1;
    }

    print_format("Input", &input_format);

    UInt32 frames = buffer_frames;
    AudioUnitSetProperty(g_input_au, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Global, 0, &frames, sizeof(frames));
    AudioUnitSetProperty(g_output_au, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Global, 0, &frames, sizeof(frames));

    int input_interleaved = !(input_format.mFormatFlags & kAudioFormatFlagIsNonInterleaved);

    UInt32 nb, cpb, bpb;
    if (input_interleaved) {
        nb = 1; cpb = input_format.mChannelsPerFrame;
        bpb = buffer_frames * input_format.mBytesPerFrame;
    } else {
        nb = input_format.mChannelsPerFrame; cpb = 1;
        bpb = buffer_frames * input_format.mBytesPerFrame;
    }

    UInt32 total = sizeof(AudioBufferList) + (nb - 1) * sizeof(AudioBuffer);
    g_meter_input_buf = (AudioBufferList *)calloc(1, total);
    g_meter_input_buf->mNumberBuffers = nb;
    for (UInt32 b = 0; b < nb; b++) {
        g_meter_input_buf->mBuffers[b].mNumberChannels = cpb;
        g_meter_input_buf->mBuffers[b].mDataByteSize = bpb;
        g_meter_input_buf->mBuffers[b].mData = calloc(1, bpb);
    }

    AURenderCallbackStruct input_cb = { .inputProc = meter_input_callback,
                                        .inputProcRefCon = NULL };
    err = AudioUnitSetProperty(g_input_au, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0,
                               &input_cb, sizeof(input_cb));
    if (err != noErr) {
        fprintf(stderr, "Failed to set input callback: %d\n", (int)err);
        return 1;
    }

    AURenderCallbackStruct silent_cb = { .inputProc = silent_render,
                                         .inputProcRefCon = NULL };
    err = AudioUnitSetProperty(g_output_au, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0,
                               &silent_cb, sizeof(silent_cb));
    if (err != noErr) {
        fprintf(stderr, "Failed to set output render callback: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitInitialize(g_input_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to initialize input unit: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitInitialize(g_output_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to initialize output unit: %d\n", (int)err);
        return 1;
    }

    err = AudioOutputUnitStart(g_input_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to start input unit: %d\n", (int)err);
        return 1;
    }

    usleep(100000);

    err = AudioOutputUnitStart(g_output_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to start output unit: %d\n", (int)err);
        return 1;
    }

    return 0;
}

static void display_meter(int num_channels) {
    int bar_width = 30;
    float peaks[2];
    peaks[0] = g_meter_peak_l;
    peaks[1] = g_meter_peak_r;

    printf("\r  ");

    int show_ch = num_channels;
    if (show_ch > 2) show_ch = 2;

    for (int ch = 0; ch < show_ch; ch++) {
        const char *label = (show_ch == 1) ? "" : (ch == 0 ? "L " : "R ");
        float db = (peaks[ch] > 0.0001f) ? 20.0f * log10f(peaks[ch]) : -80.0f;
        if (db < -80.0f) db = -80.0f;
        int filled = (int)((db + 80.0f) / 80.0f * bar_width);
        if (filled < 0) filled = 0;
        if (filled > bar_width) filled = bar_width;

        printf("%s[", label);
        for (int i = 0; i < bar_width; i++) {
            if (i < filled) {
                if (i < bar_width * 0.6)
                    printf("#");
                else if (i < bar_width * 0.85)
                    printf("=");
                else
                    printf("+");
            } else {
                printf("-");
            }
        }
        printf("] %5.1f dB  ", db);
    }

    fflush(stdout);
}

static int setup_audio(AudioDeviceID input_dev, AudioDeviceID output_dev,
                       UInt32 buffer_frames, int test_mode) {
    OSStatus err;

    if (test_mode) {
        AudioComponentDescription desc = {
            .componentType = kAudioUnitType_Output,
            .componentSubType = kAudioUnitSubType_HALOutput,
            .componentManufacturer = kAudioUnitManufacturer_Apple,
            .componentFlags = 0,
            .componentFlagsMask = 0
        };

        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        err = AudioComponentInstanceNew(comp, &g_output_au);
        if (err != noErr) {
            fprintf(stderr, "Failed to create AudioUnit: %d\n", (int)err);
            return 1;
        }

        UInt32 enable = 1;
        UInt32 disable = 0;
        AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &enable, sizeof(enable));
        AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, 1, &disable, sizeof(disable));
        AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &output_dev, sizeof(output_dev));

        AudioStreamBasicDescription fmt;
        UInt32 fs = sizeof(fmt);
        AudioUnitGetProperty(g_output_au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &fmt, &fs);
        print_format("Output", &fmt);

        UInt32 frames = buffer_frames;
        AudioUnitSetProperty(g_output_au, kAudioDevicePropertyBufferFrameSize,
                             kAudioUnitScope_Global, 0, &frames, sizeof(frames));

        RenderContext *ctx = (RenderContext *)calloc(1, sizeof(RenderContext));
        ctx->test_mode = 1;

        AURenderCallbackStruct cb = { .inputProc = render_callback, .inputProcRefCon = ctx };
        AudioUnitSetProperty(g_output_au, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof(cb));

        AudioUnitInitialize(g_output_au);
        AudioOutputUnitStart(g_output_au);
        return 0;
    }

    if (input_dev == output_dev) {
        AudioComponentDescription desc = {
            .componentType = kAudioUnitType_Output,
            .componentSubType = kAudioUnitSubType_HALOutput,
            .componentManufacturer = kAudioUnitManufacturer_Apple,
            .componentFlags = 0,
            .componentFlagsMask = 0
        };

        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        err = AudioComponentInstanceNew(comp, &g_au);
        if (err != noErr) {
            fprintf(stderr, "Failed to create AudioUnit: %d\n", (int)err);
            return 1;
        }

        UInt32 enable = 1;
        AudioUnitSetProperty(g_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &enable, sizeof(enable));
        AudioUnitSetProperty(g_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, 1, &enable, sizeof(enable));
        AudioUnitSetProperty(g_au, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &input_dev, sizeof(input_dev));

        AudioStreamBasicDescription ifmt, ofmt;
        UInt32 fs = sizeof(ifmt);
        AudioUnitGetProperty(g_au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 1, &ifmt, &fs);
        fs = sizeof(ofmt);
        AudioUnitGetProperty(g_au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &ofmt, &fs);
        print_format("Input", &ifmt);
        print_format("Output", &ofmt);

        UInt32 frames = buffer_frames;
        AudioUnitSetProperty(g_au, kAudioDevicePropertyBufferFrameSize,
                             kAudioUnitScope_Global, 0, &frames, sizeof(frames));

        RenderContext *ctx = (RenderContext *)calloc(1, sizeof(RenderContext));
        ctx->input_channels = ifmt.mChannelsPerFrame;
        ctx->input_interleaved = !(ifmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved);

        UInt32 nb, cb_val, bpb;
        if (ctx->input_interleaved) {
            nb = 1; cb_val = ifmt.mChannelsPerFrame;
            bpb = buffer_frames * ifmt.mBytesPerFrame;
        } else {
            nb = ifmt.mChannelsPerFrame; cb_val = 1;
            bpb = buffer_frames * ifmt.mBytesPerFrame;
        }

        UInt32 total = sizeof(AudioBufferList) + (nb - 1) * sizeof(AudioBuffer);
        ctx->input_buf = (AudioBufferList *)calloc(1, total);
        ctx->input_buf->mNumberBuffers = nb;
        for (UInt32 b = 0; b < nb; b++) {
            ctx->input_buf->mBuffers[b].mNumberChannels = cb_val;
            ctx->input_buf->mBuffers[b].mDataByteSize = bpb;
            ctx->input_buf->mBuffers[b].mData = calloc(1, bpb);
        }

        AURenderCallbackStruct input_cbstruct = { .inputProc = same_device_input_callback,
                                                   .inputProcRefCon = ctx };
        AudioUnitSetProperty(g_au, kAudioOutputUnitProperty_SetInputCallback,
                             kAudioUnitScope_Global, 0, &input_cbstruct, sizeof(input_cbstruct));

        AURenderCallbackStruct cbstruct = { .inputProc = render_callback, .inputProcRefCon = ctx };
        AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cbstruct, sizeof(cbstruct));

        AudioUnitInitialize(g_au);
        AudioOutputUnitStart(g_au);
        return 0;
    }

    printf("  Cross-device: creating two AudioUnits\n");

    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);

    err = AudioComponentInstanceNew(comp, &g_input_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to create input AudioUnit: %d\n", (int)err);
        return 1;
    }

    UInt32 enable = 1;

    err = AudioUnitSetProperty(g_input_au, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1,
                               &enable, sizeof(enable));
    if (err != noErr) {
        fprintf(stderr, "Failed to enable input IO: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitSetProperty(g_input_au, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &input_dev, sizeof(input_dev));
    if (err != noErr) {
        fprintf(stderr, "Failed to set input device: %d\n", (int)err);
        return 1;
    }

    comp = AudioComponentFindNext(NULL, &desc);

    err = AudioComponentInstanceNew(comp, &g_output_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to create output AudioUnit: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0,
                               &enable, sizeof(enable));
    if (err != noErr) {
        fprintf(stderr, "Failed to enable output IO: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitSetProperty(g_output_au, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &output_dev, sizeof(output_dev));
    if (err != noErr) {
        fprintf(stderr, "Failed to set output device: %d\n", (int)err);
        return 1;
    }

    AudioStreamBasicDescription input_format;
    UInt32 fmt_size = sizeof(input_format);
    err = AudioUnitGetProperty(g_input_au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 1,
                               &input_format, &fmt_size);
    if (err != noErr) {
        fprintf(stderr, "Failed to get input format: %d\n", (int)err);
        return 1;
    }

    AudioStreamBasicDescription output_format;
    fmt_size = sizeof(output_format);
    err = AudioUnitGetProperty(g_output_au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0,
                               &output_format, &fmt_size);
    if (err != noErr) {
        fprintf(stderr, "Failed to get output format: %d\n", (int)err);
        return 1;
    }

    print_format("Input", &input_format);
    print_format("Output", &output_format);

    UInt32 frames = buffer_frames;
    AudioUnitSetProperty(g_input_au, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Global, 0, &frames, sizeof(frames));
    AudioUnitSetProperty(g_output_au, kAudioDevicePropertyBufferFrameSize,
                         kAudioUnitScope_Global, 0, &frames, sizeof(frames));

    RenderContext *ctx = (RenderContext *)calloc(1, sizeof(RenderContext));
    ctx->input_channels = input_format.mChannelsPerFrame;
    ctx->input_interleaved = !(input_format.mFormatFlags & kAudioFormatFlagIsNonInterleaved);

    UInt32 nb, cpb, bpb;
    if (ctx->input_interleaved) {
        nb = 1; cpb = input_format.mChannelsPerFrame;
        bpb = buffer_frames * input_format.mBytesPerFrame;
    } else {
        nb = input_format.mChannelsPerFrame; cpb = 1;
        bpb = buffer_frames * input_format.mBytesPerFrame;
    }

    UInt32 total = sizeof(AudioBufferList) + (nb - 1) * sizeof(AudioBuffer);
    ctx->input_buf = (AudioBufferList *)calloc(1, total);
    ctx->input_buf->mNumberBuffers = nb;
    for (UInt32 b = 0; b < nb; b++) {
        ctx->input_buf->mBuffers[b].mNumberChannels = cpb;
        ctx->input_buf->mBuffers[b].mDataByteSize = bpb;
        ctx->input_buf->mBuffers[b].mData = calloc(1, bpb);
    }

    AURenderCallbackStruct input_cb = { .inputProc = route_input_callback,
                                        .inputProcRefCon = ctx };
    err = AudioUnitSetProperty(g_input_au, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0,
                               &input_cb, sizeof(input_cb));
    if (err != noErr) {
        fprintf(stderr, "Failed to set input callback: %d\n", (int)err);
        return 1;
    }

    AURenderCallbackStruct cb = { .inputProc = render_callback, .inputProcRefCon = ctx };
    err = AudioUnitSetProperty(g_output_au, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (err != noErr) {
        fprintf(stderr, "Failed to set render callback: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitInitialize(g_input_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to initialize input unit: %d\n", (int)err);
        return 1;
    }

    err = AudioUnitInitialize(g_output_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to initialize output unit: %d\n", (int)err);
        return 1;
    }

    err = AudioOutputUnitStart(g_input_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to start input unit: %d\n", (int)err);
        return 1;
    }

    usleep(100000);

    err = AudioOutputUnitStart(g_output_au);
    if (err != noErr) {
        fprintf(stderr, "Failed to start output unit: %d\n", (int)err);
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    AudioDeviceID input_dev = kAudioDeviceUnknown;
    AudioDeviceID output_dev = kAudioDeviceUnknown;
    UInt32 buffer_frames = 256;
    int do_list = 0;
    int test_mode = 0;
    int meter_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            test_mode = 1;
        } else if (strcmp(argv[i], "-m") == 0) {
            meter_mode = 1;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_dev = resolve_device(argv[++i], 1);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_dev = resolve_device(argv[++i], 0);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            buffer_frames = (UInt32)atoi(argv[++i]);
            if (buffer_frames < 32) buffer_frames = 32;
            if (buffer_frames > 4096) buffer_frames = 4096;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (do_list) {
        list_devices();
        return 0;
    }

    if (meter_mode) {
        if (input_dev == kAudioDeviceUnknown)
            input_dev = get_default_device(1);
        if (output_dev == kAudioDeviceUnknown)
            output_dev = get_default_device(0);

        char in_name[128] = "Unknown";
        get_device_name(input_dev, in_name, sizeof(in_name));
        Float64 in_rate = get_device_sample_rate(input_dev);

        char out_name[128] = "Unknown";
        get_device_name(output_dev, out_name, sizeof(out_name));
        Float64 out_rate = get_device_sample_rate(output_dev);

        printf("Audio Router - Input Meter\n");
        printf("  Input:  %s (%.0f Hz)\n", in_name, in_rate);
        printf("  Output: %s (%.0f Hz) [silent]\n", out_name, out_rate);
        printf("  Buffer: %u frames (~%.1f ms)\n", buffer_frames,
               (double)buffer_frames / (in_rate > 0 ? in_rate : 48000.0) * 1000.0);
        printf("  Press Ctrl+C to stop\n\n");

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        if (setup_meter(input_dev, output_dev, buffer_frames) != 0)
            return 1;

        int num_display_channels = 2;

        while (g_running) {
            display_meter(num_display_channels);
            usleep(50000);
        }

        printf("\n\nStopping...\n");

        if (g_output_au) {
            AudioOutputUnitStop(g_output_au);
            AudioUnitUninitialize(g_output_au);
            AudioComponentInstanceDispose(g_output_au);
        }
        if (g_input_au) {
            AudioOutputUnitStop(g_input_au);
            AudioUnitUninitialize(g_input_au);
            AudioComponentInstanceDispose(g_input_au);
        }

        printf("Done.\n");
        return 0;
    }

    if (output_dev == kAudioDeviceUnknown)
        output_dev = get_default_device(0);
    if (input_dev == kAudioDeviceUnknown)
        input_dev = get_default_device(1);

    char out_name[128] = "Unknown";
    get_device_name(output_dev, out_name, sizeof(out_name));
    Float64 out_rate = get_device_sample_rate(output_dev);

    if (test_mode) {
        printf("Audio Router - Test Tone\n");
        printf("  Output: %s (%.0f Hz)\n", out_name, out_rate);
        printf("  Buffer: %u frames (~%.1f ms)\n", buffer_frames,
               (double)buffer_frames / (out_rate > 0 ? out_rate : 48000.0) * 1000.0);
        printf("  Tone:   440 Hz sine wave\n");
        printf("  Press Ctrl+C to stop\n\n");
    } else {
        char in_name[128] = "Unknown";
        get_device_name(input_dev, in_name, sizeof(in_name));
        Float64 in_rate = get_device_sample_rate(input_dev);

        printf("Audio Router\n");
        printf("  Input:  %s (%.0f Hz)\n", in_name, in_rate);
        printf("  Output: %s (%.0f Hz)\n", out_name, out_rate);
        printf("  Buffer: %u frames (~%.1f ms)\n", buffer_frames,
               (double)buffer_frames / (out_rate > 0 ? out_rate : 48000.0) * 1000.0);
        printf("  Press Ctrl+C to stop\n\n");
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (setup_audio(input_dev, output_dev, buffer_frames, test_mode) != 0)
        return 1;

    while (g_running)
        usleep(50000);

    printf("\nStopping...\n");

    if (g_output_au) {
        AudioOutputUnitStop(g_output_au);
        AudioUnitUninitialize(g_output_au);
        AudioComponentInstanceDispose(g_output_au);
    }
    if (g_input_au) {
        AudioOutputUnitStop(g_input_au);
        AudioUnitUninitialize(g_input_au);
        AudioComponentInstanceDispose(g_input_au);
    }
    if (g_au) {
        AudioOutputUnitStop(g_au);
        AudioUnitUninitialize(g_au);
        AudioComponentInstanceDispose(g_au);
    }

    printf("Done.\n");
    return 0;
}
