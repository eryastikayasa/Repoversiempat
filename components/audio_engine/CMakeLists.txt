idf_component_register(
    SRCS "audio_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES audio_hal wakeword
)
