#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wakeword_init(void);
bool wakeword_process_pcm16(const int16_t *samples, size_t sample_count);
int wakeword_get_chunk_samples(void);
int wakeword_get_sample_rate(void);
void wakeword_deinit(void);

#ifdef __cplusplus
}
#endif
