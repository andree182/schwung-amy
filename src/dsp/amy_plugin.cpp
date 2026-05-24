/*
 * AMY DSP Plugin for Move Anything
 *
 * MIT/GPL License
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/* Plugin API definitions */
extern "C" {
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
  uint32_t api_version;
  int sample_rate;
  int frames_per_block;
  uint8_t *mapped_memory;
  int audio_out_offset;
  int audio_in_offset;
  void (*log)(const char *msg);
  int (*midi_send_internal)(const uint8_t *msg, int len);
  int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
  uint32_t api_version;
  void *(*create_instance)(const char *module_dir, const char *json_defaults);
  void (*destroy_instance)(void *instance);
  void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
  void (*set_param)(void *instance, const char *key, const char *val);
  int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
  int (*get_error)(void *instance, char *buf, int buf_len);
  void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t *(*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
}

/* AMY headers */
extern "C" {
#include "amy.h"
}

/* Host API reference */
static const host_api_v1_t *g_host = nullptr;

extern "C" void plugin_log(const char *msg) {
  if (g_host && g_host->log) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[amy_plugin] %s", msg);
    g_host->log(buf);
  }
}

/* Platform stubs for AMY */
extern "C" {
void amy_platform_init() {}
void amy_platform_deinit() {}
void amy_update_tasks() {}
int16_t *amy_render_audio() {
  amy_render(0, AMY_OSCS, 0);
  return amy_fill_buffer();
}
size_t amy_i2s_write(const uint8_t *buffer, size_t nbytes) {
  return 0;
}
}

/* =====================================================================
 * Instance structure
 * ===================================================================== */
typedef struct amy_instance_t {
  char module_dir[256];
  char error_msg[256];

  int current_preset;
  int preset_count;
  int octave_transpose;
  float output_gain;
  char preset_name[64];

  /* Pre-built JSON strings */
  char *ui_hierarchy_json;
  char *chain_params_json;

  /* Voice Management */
  struct Voice {
    int midi_note;       // -1 if free
    uint32_t last_used;  // for LRU stealing
  } voices[16];
  uint32_t note_counter;

  amy_instance_t() {
    memset(module_dir, 0, sizeof(module_dir));
    memset(error_msg, 0, sizeof(error_msg));
    current_preset = 0;
    preset_count = 1;
    octave_transpose = 0;
    output_gain = 0.5f;
    strcpy(preset_name, "Sine Synth");
    ui_hierarchy_json = nullptr;
    chain_params_json = nullptr;

    for (int i = 0; i < 16; i++) {
      voices[i].midi_note = -1;
      voices[i].last_used = 0;
    }
    note_counter = 0;
  }
} amy_instance_t;

/* =====================================================================
 * Plugin API v2 implementation
 * ===================================================================== */

static void *v2_create_instance(const char *module_dir,
                                const char *json_defaults) {
  plugin_log("Creating instance...");
  amy_instance_t *inst = new amy_instance_t();
  strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);

  // Initialize AMY globally if not running
  if (!amy_global.running) {
    plugin_log("Starting AMY engine...");
    amy_config_t config = amy_default_config();
    config.audio = AMY_AUDIO_IS_NONE;
    config.midi = AMY_MIDI_IS_NONE;
    config.platform.multicore = 0;
    config.platform.multithread = 0;
    amy_start(config);
  }

  // Pre-load the JSON metadata
  inst->ui_hierarchy_json = strdup(
    "{"
      "\"modes\":null,"
      "\"levels\":{"
        "\"root\":{"
          "\"list_param\":\"preset\","
          "\"count_param\":\"preset_count\","
          "\"name_param\":\"preset_name\","
          "\"children\":\"main\","
          "\"knobs\":[],"
          "\"params\":[]"
        "},"
        "\"main\":{"
          "\"children\":null,"
          "\"knobs\":[],"
          "\"params\":[]"
        "}"
      "}"
    "}"
  );

  inst->chain_params_json = strdup(
    "["
      "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":0},"
      "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3}"
    "]"
  );

  plugin_log("Instance created successfully.");
  return inst;
}

static void v2_destroy_instance(void *instance) {
  plugin_log("Destroying instance...");
  amy_instance_t *inst = (amy_instance_t *)instance;
  if (inst) {
    if (inst->ui_hierarchy_json) free(inst->ui_hierarchy_json);
    if (inst->chain_params_json) free(inst->chain_params_json);
    delete inst;
  }
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len,
                       int source) {
  amy_instance_t *inst = (amy_instance_t *)instance;
  if (!inst || len < 3)
    return;

  uint8_t status = msg[0] & 0xF0;
  uint8_t note = msg[1];
  uint8_t velocity = msg[2];

  int transposed_note = note + (inst->octave_transpose * 12);
  if (transposed_note < 0) transposed_note = 0;
  if (transposed_note > 127) transposed_note = 127;

  if (status == 0x90 && velocity > 0) {
    // Note On
    int voice_idx = -1;
    // 1. Find if already playing
    for (int i = 0; i < 16; i++) {
      if (inst->voices[i].midi_note == transposed_note) {
        voice_idx = i;
        break;
      }
    }
    // 2. Find free voice
    if (voice_idx == -1) {
      for (int i = 0; i < 16; i++) {
        if (inst->voices[i].midi_note == -1) {
          voice_idx = i;
          break;
        }
      }
    }
    // 3. Steal voice (LRU)
    if (voice_idx == -1) {
      uint32_t oldest = 0xFFFFFFFF;
      for (int i = 0; i < 16; i++) {
        if (inst->voices[i].last_used < oldest) {
          oldest = inst->voices[i].last_used;
          voice_idx = i;
        }
      }
    }

    if (voice_idx != -1) {
      inst->voices[voice_idx].midi_note = transposed_note;
      inst->voices[voice_idx].last_used = ++inst->note_counter;

      int osc = voice_idx;
      reset_osc(osc);

      amy_event e = amy_default_event();
      e.osc = osc;
      e.wave = SINE;
      e.midi_note = transposed_note;
      e.velocity = (float)velocity / 127.0f;
      amy_add_event(&e);
    }
  } else if (status == 0x80 || (status == 0x90 && velocity == 0)) {
    // Note Off
    for (int i = 0; i < 16; i++) {
      if (inst->voices[i].midi_note == transposed_note) {
        inst->voices[i].midi_note = -1;

        int osc = i;
        amy_event e = amy_default_event();
        e.osc = osc;
        e.velocity = 0.0f;
        amy_add_event(&e);
      }
    }
  }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
  amy_instance_t *inst = (amy_instance_t *)instance;
  if (!inst)
    return;

  if (strcmp(key, "preset") == 0) {
    inst->current_preset = atoi(val);
  } else if (strcmp(key, "octave_transpose") == 0) {
    inst->octave_transpose = atoi(val);
  } else if (strcmp(key, "all_notes_off") == 0) {
    for (int i = 0; i < 16; i++) {
      if (inst->voices[i].midi_note != -1) {
        inst->voices[i].midi_note = -1;
        reset_osc(i);
      }
    }
  }
}

static int v2_get_param(void *instance, const char *key, char *buf,
                        int buf_len) {
  amy_instance_t *inst = (amy_instance_t *)instance;
  if (!inst)
    return -1;

  if (strcmp(key, "preset") == 0)
    return snprintf(buf, buf_len, "%d", inst->current_preset);
  if (strcmp(key, "preset_count") == 0)
    return snprintf(buf, buf_len, "%d", inst->preset_count);
  if (strcmp(key, "preset_name") == 0)
    return snprintf(buf, buf_len, "%s", inst->preset_name);
  if (strcmp(key, "name") == 0)
    return snprintf(buf, buf_len, "AMY");
  if (strcmp(key, "octave_transpose") == 0)
    return snprintf(buf, buf_len, "%d", inst->octave_transpose);
  if (strcmp(key, "ui_hierarchy") == 0 && inst->ui_hierarchy_json) {
    int len = strlen(inst->ui_hierarchy_json);
    if (len < buf_len) {
      strcpy(buf, inst->ui_hierarchy_json);
      return len;
    }
    return -1;
  }
  if (strcmp(key, "chain_params") == 0 && inst->chain_params_json) {
    int len = strlen(inst->chain_params_json);
    if (len < buf_len) {
      strcpy(buf, inst->chain_params_json);
      return len;
    }
    return -1;
  }
  if (strcmp(key, "state") == 0) {
    return snprintf(buf, buf_len,
                    "{\"preset\":%d,\"octave_transpose\":%d}",
                    inst->current_preset, inst->octave_transpose);
  }

  return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
  amy_instance_t *inst = (amy_instance_t *)instance;
  if (!inst || inst->error_msg[0] == '\0')
    return 0;
  return snprintf(buf, buf_len, "%s", inst->error_msg);
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr,
                            int frames) {
  amy_instance_t *inst = (amy_instance_t *)instance;
  if (!inst) {
    memset(out_interleaved_lr, 0, frames * 4);
    return;
  }

  // Execute deltas, render voice oscillators, and generate buffer
  amy_execute_deltas();
  amy_render(0, AMY_OSCS, 0);
  int16_t *amy_buf = amy_fill_buffer();

  if (amy_buf) {
    // Interleave output and scale by output gain
    for (int i = 0; i < frames * 2; i++) {
      int32_t val = (int32_t)(amy_buf[i] * inst->output_gain);
      if (val > 32767) val = 32767;
      if (val < -32768) val = -32768;
      out_interleaved_lr[i] = (int16_t)val;
    }
  } else {
    memset(out_interleaved_lr, 0, frames * 4);
  }
}

/* =====================================================================
 * Plugin API v2 export
 * ===================================================================== */
static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
  g_host = host;

  memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
  g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
  g_plugin_api_v2.create_instance = v2_create_instance;
  g_plugin_api_v2.destroy_instance = v2_destroy_instance;
  g_plugin_api_v2.on_midi = v2_on_midi;
  g_plugin_api_v2.set_param = v2_set_param;
  g_plugin_api_v2.get_param = v2_get_param;
  g_plugin_api_v2.get_error = v2_get_error;
  g_plugin_api_v2.render_block = v2_render_block;

  return &g_plugin_api_v2;
}
