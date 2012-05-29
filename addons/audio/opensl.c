/* OpenSL: The Standard for Embedded Audio Acceleration
 * http://www.khronos.org/opensles/
 * http://www.khronos.org/registry/sles/specs/OpenSL_ES_Specification_1.1.pdf
 */

#include "allegro5/allegro.h"
#include "allegro5/internal/aintern_audio.h"

#include <SLES/OpenSLES.h>

/* Not sure if this one is needed, yet */
#include <SLES/OpenSLES_Android.h>

ALLEGRO_DEBUG_CHANNEL("opensl")

static SLObjectItf engine;

static const char * opensl_get_error_string(SLresult result)
{
    switch (result){
        case SL_RESULT_PRECONDITIONS_VIOLATED: return "Preconditions violated";
        case SL_RESULT_PARAMETER_INVALID: return "Invalid parameter";
        case SL_RESULT_MEMORY_FAILURE: return "Memory failure";
        case SL_RESULT_RESOURCE_ERROR: return "Resource error";
        case SL_RESULT_RESOURCE_LOST: return "Resource lost";
        case SL_RESULT_IO_ERROR: return "IO error";
        case SL_RESULT_BUFFER_INSUFFICIENT: return "Insufficient buffer";
        case SL_RESULT_CONTENT_CORRUPTED: return "Content corrupted";
        case SL_RESULT_CONTENT_UNSUPPORTED: return "Content unsupported";
        case SL_RESULT_CONTENT_NOT_FOUND: return "Content not found";
        case SL_RESULT_PERMISSION_DENIED: return "Permission denied";
        case SL_RESULT_FEATURE_UNSUPPORTED: return "Feature unsupported";
        case SL_RESULT_INTERNAL_ERROR: return "Internal error";
        case SL_RESULT_UNKNOWN_ERROR: return "Unknown error";
        case SL_RESULT_OPERATION_ABORTED: return "Operation aborted";
        case SL_RESULT_CONTROL_LOST: return "Control lost";
    }
    return "Unknown OpenSL error";
}

/* Only the original 'engine' object should be passed here */
static SLEngineItf getEngine(SLObjectItf engine){
    SLresult result;
    SLEngineItf interface;
    result = (*engine)->GetInterface(engine, SL_IID_ENGINE, &interface);
    if (result == SL_RESULT_SUCCESS){
        return interface;
    } else {
        ALLEGRO_ERROR("Could not get opensl engine: %s\n", opensl_get_error_string(result));
        return NULL;
    }
}

/* Create an output mixer that supports setting the volume on it */
static SLObjectItf createOutputMixer(SLEngineItf engine){
    SLresult result;
    SLObjectItf output;
    SLboolean required[1];
    SLInterfaceID ids[1];

    required[0] = SL_BOOLEAN_TRUE;
    ids[0] = SL_IID_VOLUME;

    result = (*engine)->CreateOutputMix(engine, &output, 0, ids, required);
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not create output mix: %s\n", opensl_get_error_string(result));
        return NULL;
    }

    result = (*output)->Realize(output, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not realize the output mix: %s\n", opensl_get_error_string(result));
        (*output)->Destroy(output);
        return NULL;
    }

    return output;
}

static int _opensl_open(void)
{
    SLresult result;
    SLuint32 state;
    SLEngineOption options[] = {
        (SLuint32) SL_ENGINEOPTION_THREADSAFE,
        (SLuint32) SL_BOOLEAN_TRUE,
        /*
        (SLuint32) SL_ENGINEOPTION_MAJORVERSION, (SLuint32) 1,
        (SLuint32) SL_ENGINEOPTION_MINORVERSION, (SLuint32) 1,
        */
    };

    result = slCreateEngine(&engine, 1, options, 0, NULL, NULL);
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not open audio device: %s\n",
                      opensl_get_error_string(result));
        return 1;
    }

    /* Transition the engine to the realized state in synchronous mode */
    result = (*engine)->GetState(engine, &state);
    if (result == SL_RESULT_SUCCESS){
        switch (state){
            case SL_OBJECT_STATE_UNREALIZED: {
                result = (*engine)->Realize(engine, SL_BOOLEAN_FALSE);
                break;
            }
            case SL_OBJECT_STATE_REALIZED: {
                /* This is good */
                break;
            }
            case SL_OBJECT_STATE_SUSPENDED: {
                result = (*engine)->Resume(engine, SL_BOOLEAN_FALSE);
                break;
            }
        }
    } else {
        return 1;
    }

    // output = createOutputMixer(getEngine(engine));

    return 0;
}

static void _opensl_close(void)
{
    /*
    if (output != NULL){
        (*output)->Destroy(output);
        output = NULL;
    }
    */

    if (engine != NULL){
        (*engine)->Destroy(engine);
        engine = NULL;
    }
}

typedef struct OpenSLData{
    /* Output mixer */
    SLObjectItf output;
    /* Audio player */
    SLObjectItf player;
    const void * data;
    int position;
    int length;
    int frame_size;
} OpenSLData;

static int _opensl_allocate_voice(ALLEGRO_VOICE *voice)
{
    OpenSLData * data;

    data = al_calloc(1, sizeof(*data));
    data->output = createOutputMixer(getEngine(engine));
    if (data->output == NULL){
        al_free(data);
        return 1;
    }

    data->data = NULL;
    data->player = NULL;
    data->position = 0;
    data->length = voice->buffer_size;
    data->frame_size = al_get_channel_count(voice->chan_conf) * al_get_audio_depth_size(voice->depth);

    voice->extra = data;

    return 0;
}

static void _opensl_deallocate_voice(ALLEGRO_VOICE *voice)
{
    al_free(voice->extra);
    voice->extra = NULL;
}

static int _opensl_load_voice(ALLEGRO_VOICE *voice, const void *data)
{
    OpenSLData * extra = (OpenSLData*) voice->extra;
    extra->data = data;
    extra->position = 0;

    return 0;
}

static void _opensl_unload_voice(ALLEGRO_VOICE *voice)
{
    OpenSLData * extra = (OpenSLData*) voice->extra;
    extra->data = NULL;
    extra->position = 0;
}

static SLDataFormat_PCM setupFormat(ALLEGRO_VOICE * voice){
    SLDataFormat_PCM format;
    format.formatType = SL_DATAFORMAT_PCM;

    format.numChannels = al_get_channel_count(voice->chan_conf);

    /* TODO: review the channelMasks */
    switch (voice->chan_conf){
        case ALLEGRO_CHANNEL_CONF_1: {
            /* Not sure if center is right.. */
            format.channelMask = SL_SPEAKER_FRONT_CENTER;
            break;
        }
        case ALLEGRO_CHANNEL_CONF_2: {
            format.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
            break;
        }
        case ALLEGRO_CHANNEL_CONF_3: {
            format.channelMask = SL_SPEAKER_FRONT_LEFT |
                                 SL_SPEAKER_FRONT_RIGHT |
                                 SL_SPEAKER_FRONT_CENTER;
            break;
        }
        case ALLEGRO_CHANNEL_CONF_4: {
            format.channelMask = SL_SPEAKER_FRONT_LEFT |
                                 SL_SPEAKER_BACK_LEFT |
                                 SL_SPEAKER_FRONT_RIGHT |
                                 SL_SPEAKER_BACK_RIGHT;
            break;
        }
        case ALLEGRO_CHANNEL_CONF_5_1: {
            format.channelMask = SL_SPEAKER_FRONT_LEFT |
                                 SL_SPEAKER_BACK_LEFT |
                                 SL_SPEAKER_FRONT_RIGHT |
                                 SL_SPEAKER_BACK_RIGHT |
                                 SL_SPEAKER_FRONT_CENTER |
                                 SL_SPEAKER_LOW_FREQUENCY;
            break;
        }
        case ALLEGRO_CHANNEL_CONF_6_1: {
            format.channelMask = SL_SPEAKER_FRONT_LEFT |
                                 SL_SPEAKER_BACK_LEFT |
                                 SL_SPEAKER_FRONT_RIGHT |
                                 SL_SPEAKER_BACK_RIGHT |
                                 SL_SPEAKER_FRONT_CENTER |
                                 SL_SPEAKER_LOW_FREQUENCY |
                                 SL_SPEAKER_SIDE_LEFT |
                                 SL_SPEAKER_SIDE_RIGHT;

            break;
        }
        case ALLEGRO_CHANNEL_CONF_7_1: {
            format.channelMask = SL_SPEAKER_FRONT_LEFT |
                                 SL_SPEAKER_BACK_LEFT |
                                 SL_SPEAKER_FRONT_RIGHT |
                                 SL_SPEAKER_BACK_RIGHT |
                                 SL_SPEAKER_FRONT_CENTER |
                                 SL_SPEAKER_LOW_FREQUENCY |
                                 SL_SPEAKER_SIDE_LEFT |
                                 SL_SPEAKER_SIDE_RIGHT |
                                 SL_SPEAKER_TOP_CENTER;
            break;
        }
        default: {
            ALLEGRO_ERROR("Cannot allocate voice with unknown channel configuration\n");
        }
    }

    switch (voice->frequency){
        case 8000: format.samplesPerSec = SL_SAMPLINGRATE_8; break;
        case 11025: format.samplesPerSec = SL_SAMPLINGRATE_11_025; break;
        case 12000: format.samplesPerSec = SL_SAMPLINGRATE_12; break;
        case 16000: format.samplesPerSec = SL_SAMPLINGRATE_16; break;
        case 22050: format.samplesPerSec = SL_SAMPLINGRATE_22_05; break;
        case 24000: format.samplesPerSec = SL_SAMPLINGRATE_24; break;
        case 32000: format.samplesPerSec = SL_SAMPLINGRATE_32; break;
        case 44100: format.samplesPerSec = SL_SAMPLINGRATE_44_1; break;
        case 48000: format.samplesPerSec = SL_SAMPLINGRATE_48; break;
        case 64000: format.samplesPerSec = SL_SAMPLINGRATE_64; break;
        case 88200: format.samplesPerSec = SL_SAMPLINGRATE_88_2; break;
        case 96000: format.samplesPerSec = SL_SAMPLINGRATE_96; break;
        case 192000: format.samplesPerSec = SL_SAMPLINGRATE_192; break;
        default: {
            ALLEGRO_ERROR("Unsupported frequency %d\n", voice->frequency);
        }
    }

    switch (voice->depth) {
        case ALLEGRO_AUDIO_DEPTH_UINT8:
        case ALLEGRO_AUDIO_DEPTH_INT8: {
            format.bitsPerSample = 8;
            format.containerSize = 8;
            break;
        }
        case ALLEGRO_AUDIO_DEPTH_UINT16:
        case ALLEGRO_AUDIO_DEPTH_INT16: {
            format.bitsPerSample = 16;
            format.containerSize = 16;
            break;
        }
        case ALLEGRO_AUDIO_DEPTH_UINT24:
        case ALLEGRO_AUDIO_DEPTH_INT24: {
            format.bitsPerSample = 24;
            format.containerSize = 32;
            break;
        }
        case ALLEGRO_AUDIO_DEPTH_FLOAT32: {
            format.bitsPerSample = 32;
            format.containerSize = 32;
            break;
        }
        default: {
            ALLEGRO_WARN("Cannot allocate unknown voice depth\n");
        }
    }

    /* FIXME */
    format.endianness = SL_BYTEORDER_LITTLEENDIAN;

    /*
    switch (voice->depth){
        case ALLEGRO_AUDIO_DEPTH_UINT8:
        case ALLEGRO_AUDIO_DEPTH_UINT16:
        case ALLEGRO_AUDIO_DEPTH_UINT24: {
            format.representation = SL_PCM_REPRESENTATION_UNSIGNED_INT;
        }
        case ALLEGRO_AUDIO_DEPTH_INT8:
        case ALLEGRO_AUDIO_DEPTH_INT16:
        case ALLEGRO_AUDIO_DEPTH_INT24: {
            format.representation = SL_PCM_REPRESENTATION_SIGNED_INT;
            break;
        }
        case ALLEGRO_AUDIO_DEPTH_FLOAT32: {
            format.representation = SL_PCM_REPRESENTATION_FLOAT;
            break;
        }
    }
    */

    return format;
}

static SLObjectItf createAudioPlayer(SLEngineItf engine, SLDataSource * source, SLDataSink * sink){
    SLresult result;
    SLObjectItf player;

    SLboolean required[1];
    SLInterfaceID ids[1];

    required[0] = SL_BOOLEAN_TRUE;
    ids[0] = SL_IID_BUFFERQUEUE;

    result = (*engine)->CreateAudioPlayer(engine, &player, source, sink, 1, ids, required);
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not create audio player: %s\n", opensl_get_error_string(result));
        return NULL;
    }

    result = (*player)->Realize(player, SL_BOOLEAN_FALSE);
    
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not realize audio player: %s\n", opensl_get_error_string(result));
        return NULL;
    }

    return player;
}

static void updateQueue(SLBufferQueueItf queue, void * context){
    OpenSLData * data = (OpenSLData*) context;
    if (data->position < data->length){
        int bytes = data->frame_size * 1024;
        if (data->position + bytes > data->length){
            bytes = ((data->length - data->position) / data->frame_size) * data->frame_size;
        }

        SLresult result;
        result = (*queue)->Enqueue(queue, (char*) data->data + data->position, bytes);
        data->position += bytes;
    }
}

static int _opensl_start_voice(ALLEGRO_VOICE *voice)
{
    OpenSLData * extra = (OpenSLData*) voice->extra;
    SLresult result;
    SLDataFormat_PCM format = setupFormat(voice);
    SLDataLocator_BufferQueue bufferQueue;
    SLDataSource audioSource;
    SLDataSink audioSink;
    SLBufferQueueItf queue;
    SLDataLocator_OutputMix output;
    SLVolumeItf volume;
    SLPlayItf play;

    bufferQueue.locatorType = SL_DATALOCATOR_BUFFERQUEUE;
    bufferQueue.numBuffers = 2;

    audioSource.pFormat = (void*) &format;
    audioSource.pLocator = (void*) &bufferQueue;

    output.locatorType = SL_DATALOCATOR_OUTPUTMIX;
    output.outputMix = extra->output;

    audioSink.pLocator = (void*) &output;
    audioSink.pFormat = NULL;

    /*
    result = (*extra->output)->GetInterface(extra->output, SL_IID_VOLUME, &volume);
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not get volume interface: %s\n", opensl_get_error_string(result));
        return 1;
    }
    */

    extra->player = createAudioPlayer(getEngine(engine), &audioSource, &audioSink);
    if (extra->player == NULL){
        return 1;
    }

    result = (*extra->player)->GetInterface(extra->player, SL_IID_BUFFERQUEUE, &queue);
    if (result != SL_RESULT_SUCCESS){
        ALLEGRO_ERROR("Could not get bufferqueue interface: %s\n", opensl_get_error_string(result));
        return 1;
    }

    result = (*queue)->RegisterCallback(queue, updateQueue, extra);

    // result = (*volume)->SetVolumeLevel(volume, -300);

    result = (*extra->player)->GetInterface(extra->player, SL_IID_PLAY, &play);
    result = (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);

    if (result == SL_RESULT_SUCCESS){
        ALLEGRO_DEBUG("Started new OpenSL stream\n");
    }

    return 1;
}

static int _opensl_stop_voice(ALLEGRO_VOICE* voice)
{
    /* TODO */
    ALLEGRO_ERROR("Unimplemented: _opensl_stop_voice\n");
    return 1;
}

static bool _opensl_voice_is_playing(const ALLEGRO_VOICE *voice)
{
    /* TODO */
    ALLEGRO_ERROR("Unimplemented: _opensl_voice_is_playing\n");
    return false;
}

static unsigned int _opensl_get_voice_position(const ALLEGRO_VOICE *voice)
{
    /* TODO */
    ALLEGRO_ERROR("Unimplemented: _opensl_get_voice_position\n");
    return 0;
}

static int _opensl_set_voice_position(ALLEGRO_VOICE *voice, unsigned int val)
{
    /* TODO */
    ALLEGRO_ERROR("Unimplemented: _opensl_set_voice_position\n");
    return 1;
}

ALLEGRO_AUDIO_DRIVER _al_kcm_opensl_driver = {
   "OpenSL",

   _opensl_open,
   _opensl_close,

   _opensl_allocate_voice,
   _opensl_deallocate_voice,

   _opensl_load_voice,
   _opensl_unload_voice,

   _opensl_start_voice,
   _opensl_stop_voice,

   _opensl_voice_is_playing,

   _opensl_get_voice_position,
   _opensl_set_voice_position,

   NULL,
   NULL
};
