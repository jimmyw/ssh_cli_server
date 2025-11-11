

idf_component_register(SRCS "src/ssh_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "david-cermak__libssh"
                       PRIV_REQUIRES "vfs" "esp_timer"
                       )
