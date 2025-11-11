/**
 * @file ssh_server.h
 * @brief SSH Server Library for ESP-IDF
 *
 * This library provides a complete SSH server implementation for ESP32 microcontrollers
 * using the ESP-IDF framework. It integrates with the ESP-IDF VFS (Virtual File System)
 * to provide shell access over SSH connections.
 *
 * Features:
 * - Password and public key authentication
 * - Multiple concurrent SSH sessions
 * - VFS integration for shell access
 * - Session tracking and client information
 * - Configurable shell functions
 *
 * @author ESP-IDF SSH Server Library
 * @version 1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SSH session information structure
 *
 * Contains detailed information about an active SSH session, including
 * client connection details, authentication information, and session metadata.
 * This structure is populated automatically when a client connects and
 * authenticates successfully.
 */
typedef struct {
    const char *client_ip;      ///< Client IP address (e.g., "192.168.1.100")
    uint16_t client_port;       ///< Client TCP port number
    const char *username;       ///< Authenticated username
    const char *client_version; ///< SSH client version string (e.g., "SSH-2.0-OpenSSH_8.9")
    const char *auth_method;    ///< Authentication method used ("password", "publickey", "none")
    uint32_t session_id;        ///< Unique session identifier (incremented for each connection)
    uint32_t connect_time;      ///< Connection timestamp in seconds since ESP32 boot
    bool authenticated;         ///< Whether the session has been successfully authenticated
} ssh_server_session_t;

/**
 * @brief SSH shell function callback type
 *
 * This callback function is invoked when an SSH client requests a shell session.
 * The function should implement the shell behavior (command processing, etc.)
 * and will be called in the context of a dedicated FreeRTOS task.
 *
 * @param session Pointer to session information structure containing client details
 * @param ctx User-defined context data passed from the configuration
 *
 * @note The shell function should handle its own loop and exit gracefully when
 *       the SSH channel is closed or when the client disconnects.
 */
typedef void (*ssh_shell_func_t)(ssh_server_session_t *session, void *ctx);

/**
 * @brief SSH server configuration structure
 *
 * Contains all configuration parameters needed to start and operate the SSH server.
 * This includes network settings, authentication configuration, host keys,
 * and shell behavior settings.
 */
typedef struct {
    const char *bindaddr;    ///< IP address to bind to (e.g., "0.0.0.0" for all interfaces)
    const char *port;        ///< Port number to listen on (typically "22")
    const char *debug_level; ///< Debug verbosity level for libssh (optional, can be NULL)
    const char *username;    ///< Expected username for authentication
    const char *host_key;    ///< SSH host private key in PEM format (required)

#if CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
    const char *password; ///< Expected password for password authentication
#endif

#if CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
    const char *allowed_pubkeys; ///< Authorized public keys in OpenSSH authorized_keys format
#endif

    ssh_shell_func_t shell_func; ///< Callback function to handle shell sessions
    void *shell_func_ctx;        ///< User context data passed to shell function
    uint32_t shell_task_size;    ///< Stack size in bytes for shell task (recommended: 8192)

    /**
     * @brief Whether to forcefully kill shell task on disconnect
     *
     * When true, the shell task will be forcefully terminated when the SSH channel
     * closes. When false, the shell task is expected to exit gracefully when it
     * detects the channel is closed.
     *
     * @warning Setting this to true can be dangerous if the shell task is holding
     *          resources (mutexes, file handles, etc.) as they may not be properly
     *          cleaned up. Use with caution.
     */
    bool shell_task_kill_on_disconnect;
} ssh_server_config_t;

/**
 * @brief Start the SSH server
 *
 * Initializes and starts the SSH server with the provided configuration.
 * The server will listen for incoming connections on the specified address and port,
 * handle authentication, and create shell sessions for authenticated clients.
 *
 * @param config Pointer to SSH server configuration structure
 *
 * @return
 *         - ESP_OK: Server started successfully
 *         - ESP_ERR_INVALID_ARG: Invalid configuration parameters
 *         - ESP_ERR_NO_MEM: Failed to create server task
 *
 * @note The server runs in a separate FreeRTOS task and this function returns
 *       immediately after starting the task.
 * @note Ensure the network stack is initialized and connected before calling this function.
 */
esp_err_t ssh_server_start(ssh_server_config_t *config);

/**
 * @brief Stop the SSH server
 *
 * Gracefully stops the SSH server and cleans up all resources.
 * Active SSH sessions will be terminated and the server task will exit.
 *
 * @note This function signals the server to stop but may return before
 *       the server task has completely terminated.
 */
void ssh_server_stop(void);

#ifdef __cplusplus
}
#endif