# webradio: internet radio engine (HTTP/HTTPS, HLS, MP3/AAC) + station list.
# Not part of the base system: used by the external WebRadio app.
idf_component_register(
    SRCS
        "radio.c"
        "radio_player.c"
        "hls.c"
        "stream_format.c"
        "radio_list.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        core
        services
    PRIV_REQUIRES
        hardware
        esp_http_client
        esp_audio_codec
        nvs_flash
        esp_timer
    EMBED_TXTFILES
        "radios_default.txt"
)
