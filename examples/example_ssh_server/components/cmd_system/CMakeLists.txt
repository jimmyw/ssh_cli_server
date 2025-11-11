idf_component_register(SRCS "cmd_system.c" "cmd_system_common.c" "cmd_system_shell_common.c"
                    INCLUDE_DIRS .
                    REQUIRES console spi_flash esp_driver_uart esp_driver_gpio)


