idf_component_register(
    SRCS "display.cpp" "face_animation.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_lcd esp_timer
)
