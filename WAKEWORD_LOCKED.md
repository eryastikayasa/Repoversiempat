# WakeWord Locked Contract

WakeWord is implemented as the idle-mode microphone owner.

Contract:
- ESP-SR WakeNet9 `wn9_hiesp`
- `DET_MODE_90`
- PCM16 mono 16 kHz
- AudioEngine owns the MIC lifecycle
- Supervisor consumes the latched WakeWord event
- Gemini conversation takes MIC ownership only after setupComplete

Do not introduce a second WakeWord task or a second MIC consumer.
