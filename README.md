# Repoversilima

Clean rebuild of the ESP32-S3 voice firmware.

Repo 4 (`Repoversiempat`) is reference only. Repo 5 is intentionally rebuilt from a small foundation.

Target architecture:

MIC → Audio HAL → Audio Engine → WebSocket → Gemini
Gemini → WebSocket → Audio Engine → Audio HAL → Speaker

WebSocket is transport only. Audio logic belongs to the audio layer.
