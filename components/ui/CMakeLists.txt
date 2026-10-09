# ui: theme, fonts and reusable widgets shared by every app.
idf_component_register(
    SRCS
        "ui_theme.c"
        "ui_widgets.c"
        "ui_overlay.c"
        "ui_weather.c"
        "ui_image.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl
        core
    PRIV_REQUIRES
        hardware
    # Montserrat subsets (SIL Open Font License, see fonts/OFL.txt),
    # rendered at any size by LVGL's Tiny TTF.
    EMBED_FILES
        "fonts/montserrat_light.ttf"
        "fonts/montserrat_semibold.ttf"
        "fonts/montserrat_medium_latin.ttf"
)
