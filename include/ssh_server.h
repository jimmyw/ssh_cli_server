#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *client_ip;      // Client IP address (e.g., "192.168.1.100")
    uint16_t client_port;       // Client port number
    const char *username;       // Authenticated username
    const char *client_version; // SSH client version string
    const char *auth_method;    // Authentication method used ("password", "publickey", etc.)
    uint32_t session_id;        // Unique session identifier
    uint32_t connect_time;      // Connection timestamp (seconds since boot)
    bool authenticated;         // Whether the session is authenticated
} ssh_server_session_t;

typedef void (*ssh_shell_func_t)(ssh_server_session_t *session, void *ctx);

typedef struct {
    const char *bindaddr;
    const char *port;
    const char *debug_level;
    const char *username;
    const char *host_key; // SSH host private key (PEM format)
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
void ssh_server_stop();