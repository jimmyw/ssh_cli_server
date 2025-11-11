#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*ssh_shell_func_t)(void *ctx);

typedef struct {
    const char *bindaddr;
    const char *port;
    const char *debug_level;
    const char *username;
    const char *host_key;  // SSH host private key (PEM format)
#if CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
    const char *password;
#endif
#if CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
    const char *allowed_pubkeys;
#endif
    ssh_shell_func_t shell_func;
    void *shell_func_ctx;
    uint32_t shell_task_size;
    bool shell_task_kill_on_disconnect; // Whether to kill the shell task when the SSH channel is closed, can be dangerous.
} ssh_server_config_t;

esp_err_t ssh_server_start(ssh_server_config_t *config);