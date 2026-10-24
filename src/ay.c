// AY-3-8910 model ported from kitao/playload src/engine/sound.js (MIT).
#include "ay.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOOP_UNSET INT_MIN
enum { OP_NOTE = 1, OP_REPEAT, OP_T, OP_M, OP_V, OP_W, OP_X, OP_U, OP_H };

AYProgram *ay_program_new(const double *events, int length, int fixed_volume) {
  AYProgram *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->length = length;
  p->fixed_volume = fixed_volume;
  if (length) {
    p->events = malloc(sizeof(double) * 3 * (size_t)length);
    p->loops = malloc(sizeof(int) * (size_t)length);
    if (!p->events || !p->loops) {
      ay_program_free(p);
      return NULL;
    }
    memcpy(p->events, events, sizeof(double) * 3 * (size_t)length);
    for (int i = 0; i < length; i++) p->loops[i] = LOOP_UNSET;
  }
  return p;
}

void ay_program_free(AYProgram *p) {
  if (!p) return;
  free(p->events);
  free(p->loops);
  free(p);
}

static void free_voice(AY *ay, int ch) {
  if (ay->voices[ch]) {
    ay_program_free(ay->voices[ch]->program);
    free(ay->voices[ch]);
    ay->voices[ch] = NULL;
  }
}

static void trigger(AY *ay) {
  ay->envelope_phase = 0;
  ay->envelope_step = 0;
  ay->attack = !!(ay->shape & 4);
  ay->envelope_held = 0;
  ay->envelope_level = ay->attack ? 0 : 15;
}

void ay_reset(AY *ay) {
  for (int ch = 0; ch < 3; ch++) {
    free_voice(ay, ch);
    ay->phase[ch] = 0;
    ay->tone_counter[ch] = 0;
    ay->tone_high[ch] = 1;
    ay->period[ch] = 1;
  }
  ay->mixer = 7;
  ay->tempo = 120;
  ay->noise = 0x1ffff;
  ay->noise_phase = 0;
  ay->noise_period = 1;
  ay->envelope_period = 65535;
  ay->shape = 0;
  trigger(ay);
  ay->last_input = ay->last_output = 0;
  memset(ay->history, 0, sizeof ay->history);
  ay->position = 0;
}

void ay_init(AY *ay, double sample_rate) {
  memset(ay, 0, sizeof *ay);
  ay->sample_rate = sample_rate;
  ay->clock = 1773400;
  ay->oversample = 8;
  ay->dc_decay = exp((-2 * M_PI * 20) / sample_rate);
  // Matthew Westcott's public-domain AY measurements (2001), with DC removed.
  static const double volts[16] = {1.147, 1.162, 1.169, 1.178, 1.192, 1.213, 1.238, 1.299,
                                   1.336, 1.457, 1.573, 1.707, 1.882, 2.06,  2.32,  2.58};
  for (int i = 0; i < 16; i++) ay->levels[i] = (volts[i] - volts[0]) / (volts[15] - volts[0]);
  const double cutoff = 0.4 / ay->oversample, middle = (257 - 1) / 2.0;
  double sum = 0;
  for (int i = 0; i < 257; i++) {
    const double t = i - middle, angle = (2 * M_PI * i) / (257 - 1);
    const double window = 0.42 - 0.5 * cos(angle) + 0.08 * cos(2 * angle);
    ay->kernel[i] = (t == 0 ? 2 * cutoff : sin(2 * M_PI * cutoff * t) / (M_PI * t)) * window;
    sum += ay->kernel[i];
  }
  for (int i = 0; i < 257; i++) ay->kernel[i] /= sum;
  ay_reset(ay);
}

void ay_play(AY *ay, int ch, AYProgram *program) {
  free_voice(ay, ch);
  if (!program) return;
  if (!program->length) {
    ay_program_free(program);
    return;
  }
  AYVoice *v = calloc(1, sizeof *v);
  if (!v) {
    ay_program_free(program);
    return;
  }
  v->program = program;
  v->volume = 15;
  ay->voices[ch] = v;
}

static void set_period(AY *ay, int ch, int period) {
  // The hardware keeps its half-cycle counter and output bit across a write.
  ay->period[ch] = period;
  ay->phase[ch] = ay->tone_counter[ch] + (ay->tone_high[ch] ? 0 : 8.0 * period);
}

// Returns -1 when a score loop does not advance.
static int next(AY *ay, int ch) {
  static const int shapes[8] = {0, 4, 9, 13, 8, 12, 14, 10};
  AYVoice *voice = ay->voices[ch];
  AYProgram *p = voice->program;
  int budget = 32768;
  while (voice->pc < p->length) {
    if (--budget == 0) return -1;
    const int position = voice->pc++;
    const double *e = p->events + position * 3;
    switch ((int)e[0]) {
      case OP_REPEAT: {
        const int start = (int)e[1], total = (int)e[2];
        const int count = p->loops[position] != LOOP_UNSET ? p->loops[position] : total - 1;
        if (total == 0 || count > 0) {
          p->loops[position] = count - 1;
          voice->pc = start;
        } else {
          p->loops[position] = LOOP_UNSET;
        }
        break;
      }
      case OP_T:
        if (ch == 0) ay->tempo = e[1];
        break;
      case OP_M:
        ay->mixer = (int)e[1];
        break;
      case OP_V:
        voice->volume = (int)e[1];
        voice->envelope = 0;
        break;
      case OP_W:
        ay->shape = shapes[(int)e[1] & 7];
        break;
      case OP_X:
        ay->envelope_period = e[1] > 0.5 ? e[1] : 0.5;
        break;
      case OP_U:
        voice->envelope = !p->fixed_volume;
        break;
      case OP_H:
        for (int i = 0; i < 3; i++) free_voice(ay, i);
        return 0;
      case OP_NOTE: {
        const double frequency = e[1];
        voice->remaining += e[2];
        voice->sounding = frequency > 0;
        if (voice->sounding) {
          double period = round(ay->clock / (16 * frequency));
          if (period < 1) period = 1;
          if (period > 4095) period = 4095;
          set_period(ay, ch, (int)period);
          if (ay->mixer & (8 << ch)) {
            const int n = ay->period[ch] & 31;
            ay->noise_period = n > 1 ? n : 1;
          }
          if (voice->envelope) trigger(ay);
        }
        return 0;
      }
    }
  }
  free_voice(ay, ch);
  return 0;
}

// PolyBLEP smooths square-wave edges to reduce harmonics folding below Nyquist.
static inline double edge(double phase, double step) {
  if (phase < step) {
    const double t = phase / step;
    return 2 * t - t * t - 1;
  }
  if (phase > 1 - step) {
    const double t = (phase - 1) / step;
    return t * t + 2 * t + 1;
  }
  return 0;
}

static inline double tone(AY *ay, int ch) {
  const double step = ay->clock / (16.0 * ay->period[ch] * ay->sample_rate * ay->oversample),
               phase = ay->phase[ch] / (16.0 * ay->period[ch]);
  if (step * ay->oversample >= 0.5) return 0.5;
  return (phase < 0.5 ? 1 : 0) + 0.5 * (edge(phase, step) - edge(fmod(phase + 0.5, 1.0), step));
}

static double synth(AY *ay) {
  const double clocks = ay->clock / (ay->sample_rate * ay->oversample);
  for (int ch = 0; ch < 3; ch++) {
    const double half = 8.0 * ay->period[ch], elapsed = ay->tone_counter[ch] + clocks;
    const double edges = floor(elapsed / half);
    ay->tone_counter[ch] = fmod(elapsed, half);
    if (fmod(edges, 2) != 0) ay->tone_high[ch] = !ay->tone_high[ch];
    ay->phase[ch] = ay->tone_counter[ch] + (ay->tone_high[ch] ? 0 : half);
  }
  ay->noise_phase += clocks;
  while (ay->noise_phase >= 16.0 * ay->noise_period) {
    ay->noise_phase -= 16.0 * ay->noise_period;
    ay->noise = (ay->noise >> 1) | (((ay->noise ^ (ay->noise >> 3)) & 1) << 16);
  }
  ay->envelope_phase += clocks;
  while (ay->envelope_phase >= 16.0 * ay->envelope_period) {
    ay->envelope_phase -= 16.0 * ay->envelope_period;
    if (ay->envelope_held) continue;
    ay->envelope_step++;
    if (ay->envelope_step == 16) {
      if (!(ay->shape & 8)) {
        ay->envelope_held = 1;
        ay->envelope_level = 0;
        continue;
      }
      if (ay->shape & 1) {
        ay->envelope_held = 1;
        ay->envelope_level =
            ay->shape & 2 ? (ay->attack ? 0 : 15) : ay->attack ? 15 : 0;
        continue;
      }
      if (ay->shape & 2) ay->attack = !ay->attack;
      ay->envelope_step = 0;
    }
    ay->envelope_level = ay->attack ? ay->envelope_step : 15 - ay->envelope_step;
  }
  double sum = 0;
  for (int ch = 0; ch < 3; ch++) {
    const AYVoice *voice = ay->voices[ch];
    if (!voice || !voice->sounding) continue;
    const double t = ay->mixer & (1 << ch) ? tone(ay, ch) : 1;
    const int noise = !(ay->mixer & (8 << ch)) || (ay->noise & 1);
    if (noise) sum += t * ay->levels[voice->envelope ? ay->envelope_level : voice->volume];
  }
  // Fixed DAC gain leaves headroom for three voices; author loudness with V.
  return sum * 0.2;
}

float ay_sample(AY *ay) {
  for (int ch = 0; ch < 3; ch++) {
    if (ay->voices[ch] && ay->voices[ch]->remaining <= 0 && next(ay, ch) < 0) {
      ay_reset(ay);
      return 0;
    }
    if (ay->voices[ch]) ay->voices[ch]->remaining -= ay->tempo / (60 * ay->sample_rate);
  }
  const int length = 257, half = (257 - 1) / 2;
  for (int i = 0; i < ay->oversample; i++) {
    const double value = synth(ay);
    ay->history[ay->position] = ay->history[ay->position + length] = value;
    ay->position = (ay->position + 1) % length;
  }
  // Duplicated history avoids wrapping in this hot loop. The kernel is symmetric.
  const double *h = ay->history + ay->position;
  double input = ay->kernel[half] * h[half];
  for (int i = 0; i < half; i++) input += ay->kernel[i] * (h[i] + h[length - 1 - i]);
  const double output = input - ay->last_input + ay->dc_decay * ay->last_output;
  ay->last_input = input;
  ay->last_output = output;
  return (float)output;
}
