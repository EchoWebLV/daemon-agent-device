# "main" component — the ESP-IDF entry point. PlatformIO auto-generates the
# outer CMakeLists.txt and expects this file here.
#
# ESP-IDF auto-wires the common "main" dependency chain (log, freertos,
# esp_system, heap, nvs_flash, …) so we don't need to list those explicitly.
# Add to REQUIRES only when we depend on something outside that set.
idf_component_register(
    SRCS
        "app_main.c"
        "display.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # ST7789 bring-up over SPI.
        esp_lcd
        driver
)
