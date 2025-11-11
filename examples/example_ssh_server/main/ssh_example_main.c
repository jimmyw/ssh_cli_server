/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#ifdef CONFIG_CONSOLE_SHELL_ENABLE
#include "esp_shell.h"
#include "esp_vfs_pipe.h"
#endif
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_vfs_eventfd.h"
#include "esp_vfs_fat.h"

#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "soc/soc_caps.h"

#include "cmd_nvs.h"
#include "cmd_system.h"
#include "cmd_wifi.h"
#include "console_settings.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "ssh_server.h"
#include "sys/termios.h"

/*
 * We warn if a secondary serial console is enabled. A secondary serial console is always output-only and
 * hence not very useful for interactive console applications. If you encounter this warning, consider disabling
 * the secondary serial console in menuconfig unless you know what you are doing.
 */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#if !CONFIG_ESP_CONSOLE_SECONDARY_NONE
#warning "A secondary serial console is not useful when using the console component. Please disable it in menuconfig."
#endif
#endif

static const char *TAG = "example";
#define PROMPT_STR CONFIG_IDF_TARGET

#if CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
extern const uint8_t allowed_pubkeys[] asm("_binary_ssh_allowed_client_key_pub_start");
#endif

extern const uint8_t host_key[] asm("_binary_ssh_host_ed25519_key_start");

/* Console command history can be stored to and loaded from a file.
 * The easiest way to do this is to use FATFS filesystem on top of
 * wear_levelling library.
 */
#if CONFIG_CONSOLE_STORE_HISTORY

#define MOUNT_PATH "/data"
#define HISTORY_PATH MOUNT_PATH "/history.txt"

static void initialize_filesystem(void)
{
    static wl_handle_t wl_handle;
    const esp_vfs_fat_mount_config_t mount_config = {.max_files = 4, .format_if_mount_failed = true};
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_PATH, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return;
    }
}
#else
#define HISTORY_PATH NULL
#endif // CONFIG_CONSOLE_STORE_HISTORY

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void run_linenoise_console(ssh_server_session_t *session, void *ctx)
{
    const char *prompt = (const char *)ctx;

    // Show session information when console starts
    if (session && session->client_ip) {
        printf("\r\n=== SSH Session Information ===\r\n");
        printf("Client: %s:%" PRIu16 "\r\n", session->client_ip, session->client_port);
        printf("User: %s\r\n", session->username);
        printf("Auth method: %s\r\n", session->auth_method);
        printf("Client version: %s\r\n", session->client_version);
        printf("Session ID: %" PRIu32 "\r\n", session->session_id);
        printf("Connected at: %" PRIu32 " seconds since boot\r\n", session->connect_time);
        printf("===============================\r\n\r\n");
    }

    while (1) {
        /* Get a line using linenoise.
         * The line is returned when ENTER is pressed.
         */
        char *line = linenoise(prompt);

        if (line == NULL) { /* Break on EOF or error */
            continue;
        }

        /* Add the command to the history if not empty*/
        if (strlen(line) > 0) {
            linenoiseHistoryAdd(line);
#if CONFIG_CONSOLE_STORE_HISTORY
            /* Save command history to filesystem */
            linenoiseHistorySave(HISTORY_PATH);
#endif // CONFIG_CONSOLE_STORE_HISTORY
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            linenoiseFree(line);
            break;
        }

        if (strcmp(line, "session") == 0) {
            /* Show current session information */
            if (session && session->client_ip) {
                printf("Session ID: %" PRIu32 "\r\n", session->session_id);
                printf("Client: %s:%" PRIu16 "\r\n", session->client_ip, session->client_port);
                printf("User: %s\r\n", session->username);
                printf("Auth method: %s\r\n", session->auth_method);
                printf("Client version: %s\r\n", session->client_version);
                printf("Connected: %" PRIu32 " seconds since boot\r\n", session->connect_time);
            } else {
                printf("No session information available\r\n");
            }
            linenoiseFree(line);
            continue;
        }

        /* Try to run the command */
        int ret;
#ifdef CONFIG_CONSOLE_SHELL_ENABLE
        esp_err_t err = esp_shell_run(line, &ret);
#else
        esp_err_t err = esp_console_run(line, &ret);
#endif
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            // command was empty
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        /* linenoise allocates line buffer on the heap, so need to free it */
        linenoiseFree(line);
    }
}

int log_func(const char *__restrict __fmt, __gnuc_va_list __arg)
{
    static FILE *log_file = NULL;
    if (log_file == NULL) {
        log_file = fopen("/dev/console", "w");
        if (log_file == NULL) {
            return -1;
        }
    }

    int res = vfprintf(log_file, __fmt, __arg);
    fflush(log_file);
    return res;
}

void app_main(void)
{
    esp_libc_init();
    initialize_nvs();

    // Initialize the TCP/IP stack FIRST, before any networking
    ESP_LOGI(TAG, "Initializing TCP/IP network stack...");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_log_set_vprintf(log_func);

    // This just enables the eventfd VFS feature.
    esp_vfs_eventfd_config_t config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&config));

#if CONFIG_CONSOLE_STORE_HISTORY
    initialize_filesystem();
    ESP_LOGI(TAG, "Command history enabled");
#else
    ESP_LOGI(TAG, "Command history disabled");
#endif

#ifdef CONFIG_CONSOLE_SHELL_ENABLE
    /* Configure VFS to use pipe for console I/O */
    esp_vfs_pipe_config_t cfs_config = ESP_VFS_PIPE_CONFIG_DEFAULT();
    esp_vfs_pipe_register(&cfs_config);
#endif

    /* Initialize console output periheral (UART, USB_OTG, USB_JTAG) */
    initialize_console_peripheral();

#ifdef CONFIG_VFS_SUPPORT_TERMIOS
    // Enable Ctrl+D to generate EOF on stdin
    struct termios term;
    tcgetattr(fileno(stdin), &term);
    term.c_lflag |= ICANON; // Enable canonical mode for proper EOF handling
    term.c_cc[VEOF] = 4;    // Set Ctrl+D (ASCII 4) as EOF character
    tcsetattr(fileno(stdin), TCSANOW, &term);
#endif

    /* Initialize linenoise library and esp_console*/
    initialize_console_library(HISTORY_PATH);

    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    const char *prompt = setup_prompt(PROMPT_STR ">");

    /* Register commands */
    esp_console_register_help_command();
    register_system_common();
#ifdef CONFIG_CONSOLE_COMMAND_ON_TASK
    register_system_shell_common();
#endif
#if (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_HOST_WIFI_ENABLED)
    register_wifi();
#endif
    register_nvs();

    esp_log_level_set("ssh_server", ESP_LOG_DEBUG);

    // Initialize ESP-IDF components (this will start WiFi and connect)
    ESP_LOGI(TAG, "Starting network connection...");
    ESP_ERROR_CHECK(example_connect());

    // Start SSH server
    ESP_LOGI(TAG, "Starting SSH server...");
    ssh_server_config_t server_config = {
        .bindaddr = "0.0.0.0",
        .port = "22",
        .debug_level = CONFIG_EXAMPLE_DEBUG_LEVEL,
        .username = CONFIG_EXAMPLE_DEFAULT_USERNAME,
        .host_key = (const char *)host_key,
#if CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
        .password = CONFIG_EXAMPLE_DEFAULT_PASSWORD,
#endif
#if CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
        .allowed_pubkeys = (const char *)allowed_pubkeys,
#endif
        .shell_func = run_linenoise_console,
        .shell_func_ctx = (void *)prompt,
        .shell_task_size = 8192,
        .shell_task_kill_on_disconnect = true,
    };
    ssh_server_start(&server_config);

    /* Main loop */
    run_linenoise_console(NULL, (void *)prompt);

    ESP_LOGE(TAG, "Error or end-of-input, terminating console");
    esp_console_deinit();
}
