# orbi: the Orbi family's genome (seed -> creature) and its on-screen view, shared with the companion.
idf_component_register(
    SRCS "orbi_genome.c" "orbi_view.c" "pet_sprite.c" "pokemon_pack.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES esp_hw_support
)
