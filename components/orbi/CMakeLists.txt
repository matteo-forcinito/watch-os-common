# orbi: the Orbi family's genome (seed -> creature) and its on-screen view, shared with the companion.
idf_component_register(
    SRCS "orbi_genome.c" "orbi_view.c" "pet_sprite.c" "pokemon_pack.c" "pet_pokemon.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES esp_hw_support nvs_flash
)
# Optional personal-use artwork generated with tools/prepare_pokemon.py.
# Flash-backed const data: no filesystem/cache/task is needed during rendering.
if(DEFINED ENV{WATCH_PET_POKEMON_ASSETS})
    file(TO_CMAKE_PATH "$ENV{WATCH_PET_POKEMON_ASSETS}" PET_ART)
    if(NOT EXISTS "${PET_ART}/CREDITS.json")
        message(FATAL_ERROR "Pokemon art must include its CREDITS.json")
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "${PET_ART}/pokemon_assets.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/pokemon")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WATCH_PET_POKEMON=1)
endif()
