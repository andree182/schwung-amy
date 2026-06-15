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

/* Preset Names */
#include "preset_names.h"

struct category_entry {
  const char name[64];
  int first_idx;
};

static const category_entry AMY_CATEGORIES[] = {
  { "Juno Bank A", 0 },
  { "Juno Bank B", 64 },
  { "DX7 Brass & Winds", 128 },
  { "DX7 Strings & Choir", 131 },
  { "DX7 Pianos & Keys", 135 },
  { "DX7 Guitars & Basses", 139 },
  { "DX7 Organs & Pipes", 144 },
  { "DX7 Bells & Percussion", 148 },
  { "DX7 Synth Leads", 224 },
  { "DX7 Synth Pads & FX", 240 },
  { "Other (Piano/Init)", 256 }
};
static const int AMY_CATEGORY_COUNT = sizeof(AMY_CATEGORIES) / sizeof(AMY_CATEGORIES[0]);


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

  /* Parameters */
  float filter_freq;
  float resonance;
  int filter_type;
  float volume;
  float reverb_level;
  float chorus_level;
  float echo_level;
  float echo_delay_ms;
  float echo_feedback;
  float eq_l;
  float eq_m;
  float eq_h;

  /* Pre-built JSON strings */
  char *ui_hierarchy_json;
  char *chain_params_json;
  char amy_wire[256];

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
    preset_count = 258;
    octave_transpose = 0;
    output_gain = 0.5f;
    strncpy(preset_name, AMY_PRESET_NAMES[0], sizeof(preset_name) - 1);
    ui_hierarchy_json = nullptr;
    chain_params_json = nullptr;
    memset(amy_wire, 0, sizeof(amy_wire));

    filter_freq = 8000.0f;
    resonance = 0.707f;
    filter_type = 0;
    volume = 0.5f;
    reverb_level = 0.0f;
    chorus_level = 0.0f;
    echo_level = 0.0f;
    echo_delay_ms = 500.0f;
    echo_feedback = 0.5f;
    eq_l = 0.0f;
    eq_m = 0.0f;
    eq_h = 0.0f;

    for (int i = 0; i < 16; i++) {
      voices[i].midi_note = -1;
      voices[i].last_used = 0;
    }
    note_counter = 0;
  }
} amy_instance_t;


/* =====================================================================
 * Voice Configuration helper
 * ===================================================================== */
static void load_patch_for_voices(amy_instance_t *inst, int patch_number) {
  char log_msg[256];
  snprintf(log_msg, sizeof(log_msg), "Loading patch %d (%s)...", patch_number, AMY_PRESET_NAMES[patch_number]);
  plugin_log(log_msg);

  // Clear any active notes in our allocator
  for (int i = 0; i < 16; i++) {
    inst->voices[i].midi_note = -1;
  }

  // Reset all oscillators to start fresh
  amy_event e_reset = amy_default_event();
  e_reset.reset_osc = RESET_ALL_OSCS;
  amy_add_event(&e_reset);
  amy_execute_deltas();

  // Configure voices 0..15 with patch_number under synth 0
  amy_event e = amy_default_event();
  e.patch_number = patch_number;
  e.synth = 0;
  e.num_voices = 16;
  for (int i = 0; i < 16; i++) {
    e.voices[i] = i;
  }
  amy_add_event(&e);
  amy_execute_deltas();
}

extern "C" {
extern struct synthinfo** synth;
extern uint16_t *voice_to_base_osc;
}

static void sync_params_from_engine(amy_instance_t *inst) {
  if (voice_to_base_osc && synth) {
    uint16_t base_osc = voice_to_base_osc[0];
    if (base_osc < AMY_OSCS && synth[base_osc]) {
      inst->filter_freq = freq_of_logfreq(synth[base_osc]->filter_logfreq_coefs[COEF_CONST]);
      inst->resonance = synth[base_osc]->resonance;
      inst->filter_type = synth[base_osc]->filter_type;
    }
  }
}

static int json_get_number(const char *json, const char *key, float *out) {
  if (!json || !key)
    return -1;
  char search[64];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char *pos = strstr(json, search);
  if (!pos)
    return -1;
  pos += strlen(search);
  while (*pos == ' ' || *pos == '\t')
    pos++;
  *out = (float)atof(pos);
  return 0;
}

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
    // Set max oscillators higher to comfortably support 16 voices of DX7 patches and piano (which needs 25 oscs per voice)
    config.max_oscs = 450;
    amy_start(config);

    // Pre-allocate all oscillators up to max capacity with max breakpoints to avoid any realtime memory allocation.
    uint8_t max_bps[MAX_BREAKPOINT_SETS] = {MAX_BREAKPOINTS, MAX_BREAKPOINTS};
    for (int osc = 0; osc < AMY_OSCS + AMY_NUM_BUSES; osc++) {
      ensure_osc_allocd(osc, max_bps);
    }
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
          "\"knobs\":[\"cutoff\",\"resonance\",\"volume\",\"reverb\",\"chorus\",\"echo\"],"
          "\"params\":["
            "{\"level\":\"category_jump\",\"label\":\"Jump to Category\"}"
          "]"
        "},"
        "\"main\":{"
          "\"children\":null,"
          "\"knobs\":[\"cutoff\",\"resonance\",\"volume\",\"reverb\",\"chorus\",\"echo\"],"
          "\"params\":["
            "{\"level\":\"category_jump\",\"label\":\"Jump to Category\"},"
            "{\"level\":\"filter\",\"label\":\"Filter\"},"
            "{\"level\":\"effects\",\"label\":\"Effects\"},"
            "{\"level\":\"eq\",\"label\":\"EQ\"}"
          "]"
        "},"
        "\"category_jump\":{"
          "\"label\":\"Jump to Category\","
          "\"items_param\":\"category_list\","
          "\"select_param\":\"jump_to_category\","
          "\"navigate_to\":\"root\","
          "\"children\":null,"
          "\"knobs\":[],"
          "\"params\":[]"
        "},"
        "\"filter\":{"
          "\"children\":null,"
          "\"knobs\":[\"filter_type\",\"cutoff\",\"resonance\"],"
          "\"params\":[\"filter_type\",\"cutoff\",\"resonance\"]"
        "},"
        "\"effects\":{"
          "\"children\":null,"
          "\"knobs\":[\"reverb\",\"chorus\",\"echo\",\"echo_delay\",\"echo_feedback\"],"
          "\"params\":[\"reverb\",\"chorus\",\"echo\",\"echo_delay\",\"echo_feedback\"]"
        "},"
        "\"eq\":{"
          "\"children\":null,"
          "\"knobs\":[\"eq_l\",\"eq_m\",\"eq_h\"],"
          "\"params\":[\"eq_l\",\"eq_m\",\"eq_h\"]"
        "}"
      "}"
    "}"
  );

  inst->chain_params_json = strdup(
    "["
      "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":257},"
      "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3},"
      "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":50,\"max\":15000,\"step\":10},"
      "{\"key\":\"resonance\",\"name\":\"Resonance\",\"type\":\"float\",\"min\":0.5,\"max\":16.0,\"step\":0.1},"
      "{\"key\":\"filter_type\",\"name\":\"Filter Type\",\"type\":\"enum\",\"options\":[\"None\",\"Lowpass\",\"Bandpass\",\"Highpass\",\"Lowpass 24dB\"]},"
      "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0.0,\"max\":2.0,\"step\":0.01},"
      "{\"key\":\"reverb\",\"name\":\"Reverb\",\"type\":\"float\",\"min\":0.0,\"max\":5.0,\"step\":0.05},"
      "{\"key\":\"chorus\",\"name\":\"Chorus\",\"type\":\"float\",\"min\":0.0,\"max\":5.0,\"step\":0.05},"
      "{\"key\":\"echo\",\"name\":\"Echo\",\"type\":\"float\",\"min\":0.0,\"max\":5.0,\"step\":0.05},"
      "{\"key\":\"echo_delay\",\"name\":\"Echo Delay\",\"type\":\"float\",\"min\":10,\"max\":700,\"step\":1},"
      "{\"key\":\"echo_feedback\",\"name\":\"Echo Feedback\",\"type\":\"float\",\"min\":0.0,\"max\":0.99,\"step\":0.01},"
      "{\"key\":\"eq_l\",\"name\":\"EQ Low\",\"type\":\"float\",\"min\":-15.0,\"max\":15.0,\"step\":0.5},"
      "{\"key\":\"eq_m\",\"name\":\"EQ Mid\",\"type\":\"float\",\"min\":-15.0,\"max\":15.0,\"step\":0.5},"
      "{\"key\":\"eq_h\",\"name\":\"EQ High\",\"type\":\"float\",\"min\":-15.0,\"max\":15.0,\"step\":0.5},"
      "{\"key\":\"amy_wire\",\"name\":\"Wire Command\",\"type\":\"string\"}"
    "]"
  );


  // Load default patch 0 for all voices
  load_patch_for_voices(inst, 0);
  sync_params_from_engine(inst);


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

      amy_event e = amy_default_event();
      e.voices[0] = voice_idx;
      e.midi_note = transposed_note;
      e.velocity = (float)velocity / 127.0f;
      amy_add_event(&e);
    }
  } else if (status == 0x80 || (status == 0x90 && velocity == 0)) {
    // Note Off
    for (int i = 0; i < 16; i++) {
      if (inst->voices[i].midi_note == transposed_note) {
        inst->voices[i].midi_note = -1;

        amy_event e = amy_default_event();
        e.voices[0] = i;
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

  if (strcmp(key, "state") == 0) {
    float fval;

    /* Restore preset first (sets all engine params to preset values) */
    if (json_get_number(val, "preset", &fval) == 0) {
      int patch = (int)fval;
      if (patch >= 0 && patch < 258) {
        inst->current_preset = patch;
        strncpy(inst->preset_name, AMY_PRESET_NAMES[patch], sizeof(inst->preset_name) - 1);
        load_patch_for_voices(inst, patch);
      }
    }

    if (json_get_number(val, "octave_transpose", &fval) == 0) {
      inst->octave_transpose = (int)fval;
    }

    if (json_get_number(val, "cutoff", &fval) == 0) {
      v2_set_param(instance, "cutoff", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "resonance", &fval) == 0) {
      v2_set_param(instance, "resonance", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "filter_type", &fval) == 0) {
      v2_set_param(instance, "filter_type", std::to_string((int)fval).c_str());
    }
    if (json_get_number(val, "volume", &fval) == 0) {
      v2_set_param(instance, "volume", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "reverb", &fval) == 0) {
      v2_set_param(instance, "reverb", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "chorus", &fval) == 0) {
      v2_set_param(instance, "chorus", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "echo", &fval) == 0) {
      v2_set_param(instance, "echo", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "echo_delay", &fval) == 0) {
      v2_set_param(instance, "echo_delay", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "echo_feedback", &fval) == 0) {
      v2_set_param(instance, "echo_feedback", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "eq_l", &fval) == 0) {
      v2_set_param(instance, "eq_l", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "eq_m", &fval) == 0) {
      v2_set_param(instance, "eq_m", std::to_string(fval).c_str());
    }
    if (json_get_number(val, "eq_h", &fval) == 0) {
      v2_set_param(instance, "eq_h", std::to_string(fval).c_str());
    }
    return;
  }

  if (strcmp(key, "preset") == 0) {
    int patch = atoi(val);
    if (patch >= 0 && patch < 258) {
      inst->current_preset = patch;
      strncpy(inst->preset_name, AMY_PRESET_NAMES[patch], sizeof(inst->preset_name) - 1);
      load_patch_for_voices(inst, patch);
      sync_params_from_engine(inst);
    }
  } else if (strcmp(key, "octave_transpose") == 0) {
    inst->octave_transpose = atoi(val);
  } else if (strcmp(key, "all_notes_off") == 0) {
    for (int i = 0; i < 16; i++) {
      if (inst->voices[i].midi_note != -1) {
        inst->voices[i].midi_note = -1;
        
        amy_event e = amy_default_event();
        e.voices[0] = i;
        e.velocity = 0.0f;
        amy_add_event(&e);
      }
    }
  } else if (strcmp(key, "jump_to_category") == 0) {
    int idx = atoi(val);
    if (idx >= 0 && idx < AMY_CATEGORY_COUNT) {
      int patch = AMY_CATEGORIES[idx].first_idx;
      inst->current_preset = patch;
      strncpy(inst->preset_name, AMY_PRESET_NAMES[patch], sizeof(inst->preset_name) - 1);
      load_patch_for_voices(inst, patch);
      sync_params_from_engine(inst);
    }
  } else if (strcmp(key, "cutoff") == 0) {
    float freq = atof(val);
    if (freq < 20.0f) freq = 20.0f;
    if (freq > 20000.0f) freq = 20000.0f;
    inst->filter_freq = freq;

    amy_event e = amy_default_event();
    e.synth = 0;
    e.osc = 0;
    e.filter_freq_coefs[COEF_CONST] = freq;
    amy_add_event(&e);
  } else if (strcmp(key, "resonance") == 0) {
    float res = atof(val);
    if (res < 0.5f) res = 0.5f;
    if (res > 16.0f) res = 16.0f;
    inst->resonance = res;

    amy_event e = amy_default_event();
    e.synth = 0;
    e.osc = 0;
    e.resonance = res;
    amy_add_event(&e);
  } else if (strcmp(key, "filter_type") == 0) {
    int type = atoi(val);
    if (type >= 0 && type <= 4) {
      inst->filter_type = type;

      amy_event e = amy_default_event();
      e.synth = 0;
      e.osc = 0;
      e.filter_type = type;
      amy_add_event(&e);
    }
  } else if (strcmp(key, "volume") == 0) {
    float vol = atof(val);
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 2.0f) vol = 2.0f;
    inst->volume = vol;
    inst->output_gain = vol;
  } else if (strcmp(key, "reverb") == 0) {
    float level = atof(val);
    if (level < 0.0f) level = 0.0f;
    if (level > 5.0f) level = 5.0f;
    inst->reverb_level = level;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.reverb_level = level;
    amy_add_event(&e);
  } else if (strcmp(key, "chorus") == 0) {
    float level = atof(val);
    if (level < 0.0f) level = 0.0f;
    if (level > 5.0f) level = 5.0f;
    inst->chorus_level = level;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.chorus_level = level;
    amy_add_event(&e);
  } else if (strcmp(key, "echo") == 0) {
    float level = atof(val);
    if (level < 0.0f) level = 0.0f;
    if (level > 5.0f) level = 5.0f;
    inst->echo_level = level;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.echo_level = level;
    amy_add_event(&e);
  } else if (strcmp(key, "echo_delay") == 0) {
    float delay = atof(val);
    if (delay < 10.0f) delay = 10.0f;
    if (delay > 700.0f) delay = 700.0f;
    inst->echo_delay_ms = delay;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.echo_delay_ms = delay;
    amy_add_event(&e);
  } else if (strcmp(key, "echo_feedback") == 0) {
    float fb = atof(val);
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.99f) fb = 0.99f;
    inst->echo_feedback = fb;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.echo_feedback = fb;
    amy_add_event(&e);
  } else if (strcmp(key, "eq_l") == 0) {
    float gain = atof(val);
    if (gain < -15.0f) gain = -15.0f;
    if (gain > 15.0f) gain = 15.0f;
    inst->eq_l = gain;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.eq_l = gain;
    amy_add_event(&e);
  } else if (strcmp(key, "eq_m") == 0) {
    float gain = atof(val);
    if (gain < -15.0f) gain = -15.0f;
    if (gain > 15.0f) gain = 15.0f;
    inst->eq_m = gain;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.eq_m = gain;
    amy_add_event(&e);
  } else if (strcmp(key, "eq_h") == 0) {
    float gain = atof(val);
    if (gain < -15.0f) gain = -15.0f;
    if (gain > 15.0f) gain = 15.0f;
    inst->eq_h = gain;

    amy_event e = amy_default_event();
    e.bus = 0;
    e.eq_h = gain;
    amy_add_event(&e);
  } else if (strcmp(key, "amy_wire") == 0) {
    strncpy(inst->amy_wire, val, sizeof(inst->amy_wire) - 1);
    inst->amy_wire[sizeof(inst->amy_wire) - 1] = '\0';
    char cmd_buf[1024];
    strncpy(cmd_buf, val, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    amy_add_message(cmd_buf);
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
                    "{\"preset\":%d,\"octave_transpose\":%d"
                    ",\"cutoff\":%f,\"resonance\":%f,\"filter_type\":%d"
                    ",\"volume\":%f,\"reverb\":%f,\"chorus\":%f,\"echo\":%f"
                    ",\"echo_delay\":%f,\"echo_feedback\":%f"
                    ",\"eq_l\":%f,\"eq_m\":%f,\"eq_h\":%f}",
                    inst->current_preset, inst->octave_transpose,
                    inst->filter_freq, inst->resonance, inst->filter_type,
                    inst->volume, inst->reverb_level, inst->chorus_level, inst->echo_level,
                    inst->echo_delay_ms, inst->echo_feedback,
                    inst->eq_l, inst->eq_m, inst->eq_h);
  }
  if (strcmp(key, "cutoff") == 0)
    return snprintf(buf, buf_len, "%f", inst->filter_freq);
  if (strcmp(key, "resonance") == 0)
    return snprintf(buf, buf_len, "%f", inst->resonance);
  if (strcmp(key, "filter_type") == 0)
    return snprintf(buf, buf_len, "%d", inst->filter_type);
  if (strcmp(key, "volume") == 0)
    return snprintf(buf, buf_len, "%f", inst->volume);
  if (strcmp(key, "reverb") == 0)
    return snprintf(buf, buf_len, "%f", inst->reverb_level);
  if (strcmp(key, "chorus") == 0)
    return snprintf(buf, buf_len, "%f", inst->chorus_level);
  if (strcmp(key, "echo") == 0)
    return snprintf(buf, buf_len, "%f", inst->echo_level);
  if (strcmp(key, "echo_delay") == 0)
    return snprintf(buf, buf_len, "%f", inst->echo_delay_ms);
  if (strcmp(key, "echo_feedback") == 0)
    return snprintf(buf, buf_len, "%f", inst->echo_feedback);
  if (strcmp(key, "eq_l") == 0)
    return snprintf(buf, buf_len, "%f", inst->eq_l);
  if (strcmp(key, "eq_m") == 0)
    return snprintf(buf, buf_len, "%f", inst->eq_m);
  if (strcmp(key, "eq_h") == 0)
    return snprintf(buf, buf_len, "%f", inst->eq_h);
  if (strcmp(key, "amy_wire") == 0)
    return snprintf(buf, buf_len, "%s", inst->amy_wire);

  if (strcmp(key, "category_list") == 0) {
    std::string json = "[";
    for (int i = 0; i < AMY_CATEGORY_COUNT; i++) {
      if (i > 0)
        json += ",";
      json += "{\"index\":" + std::to_string(i) + ",\"label\":\"";
      for (const char *p = AMY_CATEGORIES[i].name; *p; p++) {
        if (*p == '"' || *p == '\\') {
          json += '\\';
        }
        json += *p;
      }
      json += "\"}";
    }
    json += "]";
    int len = (int)json.size();
    if (len < buf_len) {
      strcpy(buf, json.c_str());
      return len;
    }
    return -1;
  }

  if (strcmp(key, "bank_name") == 0) {
    const char *cat_name = "Factory Presets";
    for (int i = 0; i < AMY_CATEGORY_COUNT; i++) {
      if (inst->current_preset >= AMY_CATEGORIES[i].first_idx) {
        cat_name = AMY_CATEGORIES[i].name;
      }
    }
    return snprintf(buf, buf_len, "%s", cat_name);
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
