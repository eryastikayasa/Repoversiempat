idf_component_register(
    SRCS
        "display.cpp"
        "face_animation.cpp"
        "display_driver/display_driver.cpp"
        "display_face/display_face.cpp"
        "display_text/display_text.cpp"
        "display_engine/display_engine.cpp"
    INCLUDE_DIRS
        "include"
        "display_driver"
        "display_face"
        "display_text"
        "display_engine"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_lcd esp_timer
)
