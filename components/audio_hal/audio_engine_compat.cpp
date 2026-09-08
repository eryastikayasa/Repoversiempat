#include <stdint.h>

/* Compatibility telemetry only. AudioEngine owns real playback/accounting;
 * these symbols remain for older RX logging paths until those paths are
 * simplified to consume audio_engine_turn_t directly. */
uint32_t audio_chunks_received = 0;
uint64_t audio_bytes_received = 0;
