/* IMA ADPCM encoder: 16-bit audio at 4 bits per sample, so a voice recording is small enough to send over BLE.
 * The first sample of each byte is the low nibble. The bridge (app.py) decodes this exact format. */

#ifndef BANDFLOW_ADPCM_H
#define BANDFLOW_ADPCM_H

#include <stddef.h>
#include <stdint.h>

static const int8_t ADPCM_INDEX_STEP[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t ADPCM_STEP_SIZE[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
  118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
  6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
  32767
};

struct AdpcmState {
  int16_t predictor;
  uint8_t index;
};

static inline uint8_t adpcmEncodeSample(AdpcmState &state, int16_t sample) {
  int step = ADPCM_STEP_SIZE[state.index];
  int difference = (int)sample - state.predictor;
  uint8_t code = 0;
  if (difference < 0) {
    code = 8;
    difference = -difference;
  }
  // The decoder rebuilds the same value from the code, so the predictor never drifts from it.
  int reconstructed = step >> 3;
  if (difference >= step) {
    code |= 4;
    difference -= step;
    reconstructed += step;
  }
  step >>= 1;
  if (difference >= step) {
    code |= 2;
    difference -= step;
    reconstructed += step;
  }
  step >>= 1;
  if (difference >= step) {
    code |= 1;
    reconstructed += step;
  }

  int predicted = state.predictor + ((code & 8) ? -reconstructed : reconstructed);
  if (predicted > 32767) predicted = 32767;
  if (predicted < -32768) predicted = -32768;
  state.predictor = (int16_t)predicted;

  int index = state.index + ADPCM_INDEX_STEP[code];
  if (index < 0) index = 0;
  if (index > 88) index = 88;
  state.index = (uint8_t)index;
  return code;
}

// Encodes `count` samples into (count + 1) / 2 bytes and returns that size. `state` carries over between calls.
static inline size_t adpcmEncode(const int16_t *samples, size_t count, uint8_t *out, AdpcmState &state) {
  size_t bytes = 0;
  for (size_t index = 0; index < count; index += 2) {
    uint8_t low = adpcmEncodeSample(state, samples[index]);
    uint8_t high = index + 1 < count ? adpcmEncodeSample(state, samples[index + 1]) : 0;
    out[bytes++] = (uint8_t)(low | (high << 4));
  }
  return bytes;
}

#endif
