# Deck core: PC touchbar profiles, agent state and protocol (Remote PC page, Deck Bar).
idf_component_register(
    SRCS "deck_core.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_link
    PRIV_REQUIRES ui lvgl freertos
)
