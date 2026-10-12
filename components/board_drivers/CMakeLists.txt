# Chip drivers below the board API; no UI or service dependencies.
idf_component_register(SRCS "qmi8658.c" INCLUDE_DIRS "include"
                       REQUIRES board_api esp_driver_i2c)
