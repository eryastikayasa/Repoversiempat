idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager uart_control web_config display wakeword audio_engine websocket
)
