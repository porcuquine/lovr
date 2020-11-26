#include "audio/audio.h"
#include "data/soundData.h"
#include "core/arr.h"
#include "core/ref.h"
#include "core/ref.h"
#include "core/util.h"
#include <string.h>
#include <stdlib.h>
#include "lib/miniaudio/miniaudio.h"
#include "audio/spatializer.h"

static const ma_format formats[] = {
  [SAMPLE_I16] = ma_format_s16,
  [SAMPLE_F32] = ma_format_f32
};

static const ma_format sampleSizes[] = {
  [SAMPLE_I16] = 2,
  [SAMPLE_F32] = 4
};

struct Source {
  Source* next;
  SoundData* sound;
  ma_data_converter* converter;
  uint32_t offset;
  float volume;
  bool tracked;
  bool playing;
  bool looping;
  bool spatial;
  float pose[16];
  int output_channel_count;
};

static struct {
  bool initialized;
  ma_context context;
  AudioConfig config[AUDIO_TYPE_COUNT];
  ma_device devices[AUDIO_TYPE_COUNT];
  ma_mutex locks[AUDIO_TYPE_COUNT];
  Source* sources;
  arr_t(ma_data_converter) converters;
  Spatializer* spatializer;
} state;

// Device callbacks

static bool mix(Source* source, float* output, uint32_t count) {
  float raw[2048];
  float aux[2048];
  float mix[4096];

  // TODO
  // frameLimitIn =
  // frameLimitOut =

  while (count > 0) {
    uint32_t chunk = MIN(sizeof(raw) / (sampleSizes[source->sound->format] * source->sound->channels),
        ma_data_converter_get_required_input_frame_count(source->converter, count));
        // ^^^ Note need to min `count` with 'capacity of aux buffer' and 'capacity of mix buffer'
        // could skip min-ing with one of the buffers if you can guarantee that one is bigger/equal to the other (you can because their formats are known)

    uint64_t framesIn = source->sound->read(source->sound, source->offset, chunk, raw);
    uint64_t framesOut = sizeof(aux) / (sizeof(float) * source->output_channel_count);

    ma_data_converter_process_pcm_frames(source->converter, raw, &framesIn, aux, &framesOut);

    if(source->spatial) {
      if(state.spatializer) {
        state.spatializer->apply(source, source->pose, aux, mix, framesOut);
      }
    } else {
      memcpy(mix, aux, framesOut * 2 * sizeof(float));
    }

    for (uint32_t i = 0; i < framesOut * 2; i++) {
      output[i] += mix[i] * source->volume;
    }

    if (framesIn == 0) {
      source->offset = 0;
      if (!source->looping) {
        source->playing = false;
        return false;
      }
    } else {
      source->offset += framesIn;
    }

    count -= framesOut;
    output += framesOut * 2;
  }

  return true;
}

static void onPlayback(ma_device* device, void* output, const void* _, uint32_t count) {
  ma_mutex_lock(&state.locks[AUDIO_PLAYBACK]);

  // For each Source, remove it if it isn't playing or process it and remove it if it stops
  for (Source** list = &state.sources, *source = *list; source != NULL; source = *list) {
    if (source->playing && mix(source, output, count)) {
      list = &source->next;
    } else {
      *list = source->next;
      source->tracked = false;
      lovrRelease(Source, source);
    }
  }

  ma_mutex_unlock(&state.locks[AUDIO_PLAYBACK]);
}

static void onCapture(ma_device* device, void* output, const void* input, uint32_t frames) {
  
}

static const ma_device_callback_proc callbacks[] = { onPlayback, onCapture };

static Spatializer *spatializers[] = {
  &dummy_spatializer,
};

// Entry

bool lovrAudioInit(AudioConfig config[2]) {
  if (state.initialized) return false;

  memcpy(state.config, config, sizeof(state.config));

  if (ma_context_init(NULL, 0, NULL, &state.context)) {
    return false;
  }

  lovrAudioReset();

  for (size_t i = 0; i < AUDIO_TYPE_COUNT; i++) {
    if (config[i].enable) {
      if (ma_mutex_init(&state.context, &state.locks[i])) {
        lovrAudioDestroy();
        return false;
      }

      if (config[i].start) {
        if (ma_device_start(&state.devices[i])) {
          lovrAudioDestroy();
          return false;
        }
      }
    }
  }

  for (size_t i = 0; i < sizeof(spatializers) / sizeof(spatializers[0]); i++) {
    if (spatializers[i]->init()) {
      state.spatializer = spatializers[i];
      break;
    }
  }

  arr_init(&state.converters);

  return state.initialized = true;
}

void lovrAudioDestroy() {
  if (!state.initialized) return;
  ma_device_uninit(&state.devices[AUDIO_PLAYBACK]);
  ma_device_uninit(&state.devices[AUDIO_CAPTURE]);
  ma_mutex_uninit(&state.locks[AUDIO_PLAYBACK]);
  ma_mutex_uninit(&state.locks[AUDIO_CAPTURE]);
  ma_context_uninit(&state.context);
  if (state.spatializer) state.spatializer->destroy();
  arr_free(&state.converters);
  memset(&state, 0, sizeof(state));
}

bool lovrAudioReset() {
  for (size_t i = 0; i < AUDIO_TYPE_COUNT; i++) {
    if (state.config[i].enable) {
      ma_device_type deviceType = (i == 0) ? ma_device_type_playback : ma_device_type_capture;

      ma_device_config config = ma_device_config_init(deviceType);
      config.sampleRate = LOVR_AUDIO_SAMPLE_RATE;
      config.playback.format = ma_format_f32;
      config.capture.format = ma_format_f32;
      config.playback.channels = 2;
      config.capture.channels = 1;
      config.dataCallback = callbacks[i];
      config.performanceProfile = ma_performance_profile_low_latency;

      if (ma_device_init(&state.context, &config, &state.devices[i])) {
        return false;
      }
    }
  }

  return true;
}

bool lovrAudioStart(AudioType type) {
  return !ma_device_start(&state.devices[type]);
}

bool lovrAudioStop(AudioType type) {
  return !ma_device_stop(&state.devices[type]);
}

float lovrAudioGetVolume() {
  float volume = 0.f;
  ma_device_get_master_volume(&state.devices[AUDIO_PLAYBACK], &volume);
  return volume;
}

void lovrAudioSetVolume(float volume) {
  ma_device_set_master_volume(&state.devices[AUDIO_PLAYBACK], volume);
}

void lovrAudioSetListenerPose(float position[4], float orientation[4])
{
  state.spatializer->setListenerPose(position, orientation);
}

// Source

static void _lovrSourceAssignConverter(Source *source) {
  source->converter = NULL;
  for (size_t i = 0; i < state.converters.length; i++) {
    ma_data_converter* converter = &state.converters.data[i];
    if (converter->config.formatIn != formats[source->sound->format]) continue;
    if (converter->config.sampleRateIn != source->sound->sampleRate) continue;
    if (converter->config.channelsIn != source->sound->channels) continue;
    if (converter->config.channelsOut != source->output_channel_count) continue;
    source->converter = converter;
    break;
  }

  if (!source->converter) {
    ma_data_converter_config config = ma_data_converter_config_init_default();
    config.formatIn = formats[source->sound->format];
    config.formatOut = ma_format_f32;
    config.channelsIn = source->sound->channels;
    config.channelsOut = source->output_channel_count;
    config.sampleRateIn = source->sound->sampleRate;
    config.sampleRateOut = LOVR_AUDIO_SAMPLE_RATE;
    arr_expand(&state.converters, 1);
    ma_data_converter* converter = &state.converters.data[state.converters.length++];
    lovrAssert(!ma_data_converter_init(&config, converter), "Problem creating Source data converter");
    source->converter = converter;
  }
}

Source* lovrSourceCreate(SoundData* sound, bool spatial) {
  Source* source = lovrAlloc(Source);
  source->sound = sound;
  lovrRetain(source->sound);
  source->volume = 1.f;
  
  source->spatial = spatial;
  source->output_channel_count = source->spatial ? 1 : 2;
  _lovrSourceAssignConverter(source);

  return source;
}

void lovrSourceDestroy(void* ref) {
  Source* source = ref;
  lovrRelease(SoundData, source->sound);
}

void lovrSourcePlay(Source* source) {
  ma_mutex_lock(&state.locks[AUDIO_PLAYBACK]);

  source->playing = true;

  if (!source->tracked) {
    lovrRetain(source);
    source->tracked = true;
    source->next = state.sources;
    state.sources = source;
  }

  ma_mutex_unlock(&state.locks[AUDIO_PLAYBACK]);
}

void lovrSourcePause(Source* source) {
  source->playing = false;
}

void lovrSourceStop(Source* source) {
  lovrSourcePause(source);
  lovrSourceSetTime(source, 0);
}

bool lovrSourceIsPlaying(Source* source) {
  return source->playing;
}

bool lovrSourceIsLooping(Source* source) {
  return source->looping;
}

void lovrSourceSetLooping(Source* source, bool loop) {
  source->looping = loop;
}

float lovrSourceGetVolume(Source* source) {
  return source->volume;
}

void lovrSourceSetVolume(Source* source, float volume) {
  ma_mutex_lock(&state.locks[AUDIO_PLAYBACK]);
  source->volume = volume;
  ma_mutex_unlock(&state.locks[AUDIO_PLAYBACK]);
}

bool lovrSourceGetSpatial(Source *source) {
  return source->spatial;
}

void lovrSourceSetPose(Source *source, float position[4], float orientation[4]) {
  ma_mutex_lock(&state.locks[AUDIO_PLAYBACK]);
  mat4_identity(source->pose);
  mat4_translate(source->pose, position[0], position[1], position[2]);
  mat4_rotate(source->pose, orientation[0], orientation[1], orientation[2], orientation[3]);
  ma_mutex_unlock(&state.locks[AUDIO_PLAYBACK]);
}

uint32_t lovrSourceGetTime(Source* source) {
  return source->offset;
}

void lovrSourceSetTime(Source* source, uint32_t time) {
  ma_mutex_lock(&state.locks[AUDIO_PLAYBACK]);
  source->offset = time;
  ma_mutex_unlock(&state.locks[AUDIO_PLAYBACK]);
}

SoundData* lovrSourceGetSoundData(Source* source) {
  return source->sound;
}

// Capture

uint32_t lovrAudioGetCaptureSampleCount()
{

}

struct SoundData* lovrAudioCapture(uint32_t sampleCount, SoundData *soundData, uint32_t offset)
{

}