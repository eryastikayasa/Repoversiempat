# WebSocket

WebSocket is transport only.

Architecture:

AudioEngine → central WebSocket TX worker → Gemini
Gemini → WebSocket RX worker → AudioEngine

All MIC ownership remains in AudioEngine. Gemini audio is gated by setupComplete and transport state.
