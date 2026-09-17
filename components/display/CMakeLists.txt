idf_component_register(
    SRCS
        "display.cpp"
        "face_animation.cpp"
        "display_driver/display_driver.cpp"
        "display_face/display_face.cpp"
    INCLUDE_DIRS
        "include"
        "display_driver"
        "display_face"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_lcd esp_timer
)
