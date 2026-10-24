#pragma once
// AY-3-8910 model ported from kitao/playload src/engine/sound.js (MIT).
// Three square-wave channels, shared 17-bit noise, shared 16-step envelope,
// eight-times oversampling and a 257-tap windowed-sinc output filter.

typedef struct AYProgram {
  int length;      // events
  double *events;  // [op, a, b] per event, as flattened by js/engine.js
  int fixed_volume;
  int *loops;      // per-event loop counters, LOOP_UNSET when absent
} AYProgram;

typedef struct {
  AYProgram *program;
  int pc;
  double remaining;
  int volume, envelope, sounding;
} AYVoice;

typedef struct AY {
  double sample_rate, clock;
  int oversample;
  double dc_decay;
  double levels[16];
  double kernel[257];
  double history[514];
  int position;
  AYVoice *voices[3];
  double phase[3], tone_counter[3];
  int tone_high[3], period[3];
  int mixer;
  double tempo;
  unsigned noise;
  double noise_phase;
  int noise_period;
  double envelope_period;
  int shape;
  double envelope_phase;
  int envelope_step, attack, envelope_held, envelope_level;
  double last_input, last_output;
} AY;

void ay_init(AY *ay, double sample_rate);
void ay_reset(AY *ay);
// Takes ownership of program (or frees it). length 0 silences the channel.
void ay_play(AY *ay, int ch, AYProgram *program);
float ay_sample(AY *ay);
AYProgram *ay_program_new(const double *events, int length, int fixed_volume);
void ay_program_free(AYProgram *program);
