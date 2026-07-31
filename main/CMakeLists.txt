idf_component_register(SRCS "bt_classic_work.c" "main.c"
                       REQUIRES esp_driver_gpio
                       REQUIRES bt
                       PRIV_REQUIRES bt nvs_flash
                       INCLUDE_DIRS ".")
