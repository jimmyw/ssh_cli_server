idf_component_register(SRCS "ssh_example_main.c" "console_settings.c"
                       PRIV_REQUIRES console esp_driver_uart fatfs esp_driver_usb_serial_jtag
                                     nvs_flash cmd_system cmd_wifi cmd_nvs protocol_examples_common ssh_cli_server
                       INCLUDE_DIRS "."
                       EMBED_FILES ssh_host_ed25519_key ssh_allowed_client_key.pub
                       )

