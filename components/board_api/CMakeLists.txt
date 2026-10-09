# board_api: the contract between the shared watch OS and a board.
#
# Only headers. Every firmware project has its own `hardware` component that
# implements these functions for its chips (display, touch, power, audio...);
# core, services and apps include them and never see pins or chip names.
idf_component_register(
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        sdmmc
)
