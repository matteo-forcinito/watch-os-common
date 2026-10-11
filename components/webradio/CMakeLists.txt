# webradio: internet radio engine (HTTP/HTTPS, HLS, MP3/AAC) + station list.
# + the Radio app (radio_app.h): internal on some boards, the external WebRadio app on others.
idf_component_register(
    SRCS
        "radio.c"
        "radio_player.c"
        "hls.c"
        "stream_format.c"
        "radio_list.c"
        "radio_app.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        core
        services
    PRIV_REQUIRES
        hardware
        ui
        esp_http_client
        esp_audio_codec
        nvs_flash
        esp_timer
    EMBED_TXTFILES
        "radios_default.txt"
)
