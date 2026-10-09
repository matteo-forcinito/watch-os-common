# core: the small "operating system" every app relies on
# (LVGL port, power manager, app manager, settings, shared state, clock).
idf_component_register(
    SRCS
        "lv_port.c"
        "state.c"
        "settings.c"
        "power.c"
        "app_manager.c"
        "clock.c"
        "sys.c"
        "lv_mem_psram.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        hardware
        lvgl
        esp_timer
    PRIV_REQUIRES
        nvs_flash
        espcoredump
        esp_pm
        esp_hw_support
)

# LVGL calls the allocator in lv_mem_psram.c, but LVGL is linked after core:
# make the linker take that object file anyway.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
