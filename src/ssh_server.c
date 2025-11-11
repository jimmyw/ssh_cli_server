

#include <arpa/inet.h>
#include <ctype.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_eventfd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"

#include "ssh_server.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define MAX_SSH_CHANNELS 10
#define MAX_SSH_SIGNALS 3
#define WRITE_BUFFER_SIZE 256
#define READ_BUFFER_SIZE 256
#define VFS_FD_TO_CHANNEL_INDEX(fd) ((fd) >> 1)
#define VFS_FD_IS_WRITE(fd) ((fd) & 1)
#define VFS_FD_IS_READ(fd) (!((fd) & 1))
#define CHANNEL_INDEX_TO_VFS_FD(ch_idx, is_write) (((ch_idx) << 1) | ((is_write) ? 1 : 0))

static const char *TAG = "ssh_server";

static esp_vfs_id_t s_pipe_vfs_id = -1;
static int wakeup_eventfd = -1;                  // Event FD for waking up SSH event loop
static const char *last_auth_method = "unknown"; // Track the last authentication method used

typedef struct {
    esp_vfs_select_sem_t sem;
    fd_set *read_fds;
    fd_set *write_fds;
    fd_set *error_fds;
} signal_context_t;

typedef struct {
    ssh_channel channel;
    int stdin_fd;
    int stdout_fd;
    // int stderr_fd;
    TaskHandle_t shell_task_handle;
    MessageBufferHandle_t read_buffer;  // For SSH → VFS data flow
    MessageBufferHandle_t write_buffer; // For VFS → SSH data flow
    signal_context_t signals[MAX_SSH_SIGNALS];
    ssh_server_config_t *config;
    struct ssh_channel_callbacks_struct *channel_cb;
    ssh_server_session_t session;
} ssh_vfs_context_t;

// Mapped to local fd
static ssh_vfs_context_t channels[MAX_SSH_CHANNELS];
#define CHANNEL_INDEX_FROM_PTR(ctx) ((ctx) - channels)

static void trigger_select_for_channel(int fd, bool read, bool write, bool except);

/**
 * @brief Populate session information from SSH session
 */
static void populate_session_info(ssh_server_session_t *session_info, ssh_session ssh_session, ssh_server_config_t *config)
{
    static uint32_t session_counter = 0;

    // Initialize all fields to safe defaults
    memset(session_info, 0, sizeof(ssh_server_session_t));

    // Get client IP and port
    socket_t sock = ssh_get_fd(ssh_session);
    if (sock != SSH_INVALID_SOCKET) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        if (getpeername(sock, (struct sockaddr *)&client_addr, &addr_len) == 0) {
            if (client_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in *)&client_addr;
                // Store IP in a static buffer (note: this is not thread-safe for multiple sessions)
                static char ip_buffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr_in->sin_addr, ip_buffer, sizeof(ip_buffer));
                session_info->client_ip = ip_buffer;
                session_info->client_port = ntohs(addr_in->sin_port);
            }
            // Note: IPv6 support could be added here if needed
        }
    }

    // Set other session information
    session_info->username = config->username; // The username they authenticated as
    session_info->session_id = ++session_counter;
    session_info->connect_time = esp_timer_get_time() / 1000000; // Convert microseconds to seconds
    session_info->authenticated = true;                          // If we get to channel_open, they're authenticated

    // Get client version if available
    const char *client_banner = ssh_get_clientbanner(ssh_session);
    if (client_banner) {
        static char version_buffer[128];
        strncpy(version_buffer, client_banner, sizeof(version_buffer) - 1);
        version_buffer[sizeof(version_buffer) - 1] = '\0';
        session_info->client_version = version_buffer;
    } else {
        session_info->client_version = "unknown";
    }

    // Set auth method - use the tracked method from authentication
    session_info->auth_method = last_auth_method;

    ESP_LOGI(TAG, "Session info populated - Client: %s:%u, User: %s, Auth: %s, Version: %s, ID: %u",
             session_info->client_ip ? session_info->client_ip : "unknown", session_info->client_port, session_info->username, session_info->auth_method,
             session_info->client_version, session_info->session_id);
}

// Call with NULL to find a free spot.
static ssh_vfs_context_t *allocate_new_channel_context()
{
    for (int i = 0; i < ARRAY_SIZE(channels); i++) {
        ssh_vfs_context_t *ctx = &channels[i];
        if (!ctx->channel) {
            return ctx;
        }
    }
    return NULL;
}

/**
 * @brief Drain write buffers and send data to SSH channels
 *
 * This function is called from the SSH event loop to check all channels
 * for pending write data and send it. This ensures all SSH operations
 * happen from the same thread context.
 */
static void drain_write_buffers(void)
{
    char write_data[WRITE_BUFFER_SIZE];

    for (int i = 0; i < ARRAY_SIZE(channels); i++) {
        ssh_vfs_context_t *ctx = &channels[i];

        if (ctx->channel == NULL || ctx->write_buffer == NULL) {
            continue;
        }

        // Check if channel is still open
        if (!ssh_channel_is_open(ctx->channel) || ssh_channel_is_eof(ctx->channel)) {
            trigger_select_for_channel(ctx->stdout_fd, false, false, true);
            continue;
        }

        // Try to read data from write buffer (non-blocking)
        size_t bytes_received = xMessageBufferReceive(ctx->write_buffer, write_data, sizeof(write_data),
                                                      0 // Non-blocking
        );

        if (bytes_received > 0) {
            // Send data to SSH channel (we're in the SSH thread context now!)
            int bytes_written = ssh_channel_write(ctx->channel, write_data, bytes_received);

            if (bytes_written < 0) {
                ESP_LOGW(TAG, "SSH channel write failed: %d", bytes_written);
            } else if (bytes_written < bytes_received) {
                ESP_LOGW(TAG, "Partial SSH write: %d/%zu bytes", bytes_written, bytes_received);
                // TODO: Handle partial writes by putting remaining data back in buffer
            } else {
                ESP_LOGD(TAG, "Wrote %d bytes to SSH channel from event loop", bytes_written);
                trigger_select_for_channel(ctx->stdout_fd, false, true, false);
            }
        } else {
            ESP_LOGD(TAG, "No data to write for channel %d", i);
        }
    }
}

static void ssh_shell(void *arg)
{
    ssh_vfs_context_t *ctx = (ssh_vfs_context_t *)arg;
    ssh_channel channel = ctx->channel;
    FILE *new_stdin = NULL;
    FILE *new_stdout = NULL;

    ESP_LOGD(TAG, "Shell created, setting stdout and stdin to fd %d, %d", ctx->stdout_fd, ctx->stdin_fd);

    FILE *orig_stdin = __getreent()->_stdin;
    FILE *orig_stdout = __getreent()->_stdout;
    FILE *orig_stderr = __getreent()->_stderr;
    new_stdin = fdopen(ctx->stdin_fd, "r");
    if (!new_stdin) {
        ESP_LOGE(TAG, "Failed to fdopen stdin for fd %d errno: %d", ctx->stdin_fd, errno);
        goto bail_out;
    }
    new_stdout = fdopen(ctx->stdout_fd, "w");
    if (!new_stdout) {
        ESP_LOGD(TAG, "Failed to fdopen stdout for fd %d errno: %d", ctx->stdout_fd, errno);
        goto bail_out;
    }

    __getreent()->_stdin = new_stdin;
    __getreent()->_stdout = new_stdout;

    if (ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
        ctx->config->shell_func(&ctx->session, ctx->config->shell_func_ctx);
    }
bail_out:
    __getreent()->_stdin = orig_stdin;
    __getreent()->_stdout = orig_stdout;
    __getreent()->_stderr = orig_stderr;
    if (new_stdin)
        fclose(new_stdin);
    if (new_stdout)
        fclose(new_stdout);
    ctx->shell_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief SSH shell request callback
 *
 * This callback is invoked when an SSH client requests a shell session.
 * It's called after successful authentication and channel establishment.
 * The shell request indicates that the client wants an interactive shell
 * environment (like bash, sh, etc.) rather than executing a single command.
 *
 * @param session The SSH session
 * @param channel The SSH channel for this shell session
 * @param userdata User-defined data (not used here)
 * @return SSH_OK to accept the shell request, SSH_ERROR to reject it
 */
static int shell_request(ssh_session session, ssh_channel channel, void *userdata)
{
    ESP_LOGD(TAG, "Shell requested");
    ssh_vfs_context_t *ctx = (ssh_vfs_context_t *)userdata;
    int ch_idx = CHANNEL_INDEX_FROM_PTR(ctx);

    ESP_LOGD(TAG, "Channel %d registering VFS fd", ch_idx);

    esp_err_t res = esp_vfs_register_fd_with_local_fd(s_pipe_vfs_id, CHANNEL_INDEX_TO_VFS_FD(ch_idx, false), false, &ctx->stdin_fd);
    if (res != ESP_OK) {
        ESP_LOGD(TAG, "Failed to register VFS fd: %d", res);
        return SSH_ERROR;
    }
    res = esp_vfs_register_fd_with_local_fd(s_pipe_vfs_id, CHANNEL_INDEX_TO_VFS_FD(ch_idx, true), false, &ctx->stdout_fd);
    if (res != ESP_OK) {
        ESP_LOGD(TAG, "Failed to register VFS fd: %d", res);
        return SSH_ERROR;
    }

    ESP_LOGD(TAG, "Shell setup completed successfully");
    xTaskCreate(&ssh_shell, "ssh_shell", ctx->config->shell_task_size, (void *)ctx, 5, &ctx->shell_task_handle);
    return SSH_OK;
}

/**
 * @brief SSH channel data callback
 *
 * This callback is invoked when data is received on an SSH channel.
 * It feeds the data into the message buffer for the VFS layer to consume.
 * This provides thread-safe communication between the SSH event loop
 * and the shell task.
 *
 * @param session The SSH session
 * @param channel The SSH channel that received data
 * @param data Pointer to the received data
 * @param len Length of the received data
 * @param is_stderr Whether this is stderr data (unused)
 * @param userdata User-defined data (unused)
 * @return Number of bytes processed (should return len)
 */
static int channel_data(ssh_session session, ssh_channel channel, void *data, uint32_t len, int is_stderr, void *userdata)
{
    ssh_vfs_context_t *ctx = (ssh_vfs_context_t *)userdata;
    (void)session;
    (void)is_stderr;
    (void)userdata;

    // Send data to message buffer (non-blocking), blocking time max
    size_t bytes_sent = xMessageBufferSend(ctx->read_buffer, data, len, portMAX_DELAY);

    if (bytes_sent != len) {
        ESP_LOGW(TAG, "Message buffer full, sent %zu/%u bytes", bytes_sent, len);
    } else {
        ESP_LOGD(TAG, "Sent %u bytes to read_buffer", len);
    }
    trigger_select_for_channel(ctx->stdin_fd, true, false, false);

    return len; // Always return len to libssh (we handled what we could)
}

/**
 * @brief SSH PTY (Pseudo Terminal) request callback
 *
 * This callback is invoked when an SSH client requests a pseudo terminal.
 * A PTY is needed for interactive sessions where the client expects terminal
 * features like cursor positioning, colors, line editing, etc.
 * Most SSH clients (like OpenSSH) will request a PTY before requesting a shell.
 *
 * @param session The SSH session
 * @param channel The SSH channel for this PTY
 * @param term Terminal type (e.g., "xterm", "vt100", "ansi")
 * @param cols Number of columns in the terminal
 * @param rows Number of rows in the terminal
 * @param py Pixel height (usually 0 for character terminals)
 * @param px Pixel width (usually 0 for character terminals)
 * @param userdata User-defined data (not used here)
 * @return SSH_OK to accept the PTY request, SSH_ERROR to reject it
 */
static int pty_request(ssh_session session, ssh_channel channel, const char *term, int cols, int rows, int py, int px, void *userdata)
{
    ESP_LOGD(TAG, "PTY requested: %s (%dx%d)", term, cols, rows);
    return SSH_OK;
}

#ifdef CONFIG_VFS_SUPPORT_SELECT

static void trigger_select_for_channel(int fd, bool read, bool write, bool except)
{
    for (int i = 0; i < ARRAY_SIZE(channels); i++) {
        ssh_vfs_context_t *ctx = &channels[i];
        if (ctx->channel == NULL) {
            continue;
        }

        for (int j = 0; j < ARRAY_SIZE(ctx->signals); j++) {
            signal_context_t *select_args = &ctx->signals[j];
            if (!select_args->sem.sem) {
                continue;
            }

            if (write && select_args->write_fds && FD_ISSET(fd, select_args->write_fds)) {
                esp_vfs_select_triggered(select_args->sem);
            } else if (read && select_args->read_fds && FD_ISSET(fd, select_args->read_fds)) {
                esp_vfs_select_triggered(select_args->sem);
            } else if (except && select_args->error_fds && FD_ISSET(fd, select_args->error_fds)) {
                esp_vfs_select_triggered(select_args->sem);
            }
        }
    }
}

/**
 * @brief Start select operation for SSH channels
 *
 * This function is called when select() is invoked on SSH file descriptors.
 * It checks the message buffers to determine if data is available for reading
 * or if there's space for writing, and signals the select semaphore accordingly.
 *
 * @param nfds Highest numbered file descriptor + 1
 * @param readfds Set of fds to check for reading
 * @param writefds Set of fds to check for writing
 * @param exceptfds Set of fds to check for exceptions
 * @param signal_sem Semaphore to signal when fd becomes ready
 * @param end_select_args Context to pass to end_select
 * @return ESP_OK on success
 */
static esp_err_t ssh_vfs_start_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, esp_vfs_select_sem_t signal_sem, void **end_select_args)
{
    // Check each fd in the sets
    for (int i = 0; i < nfds && i < ARRAY_SIZE(channels); i++) {
        bool fd_ready = false;
        ssh_vfs_context_t *ctx = &channels[i];
        signal_context_t *sig_ctx = NULL;

        if (ctx->channel == NULL) {
            continue;
        }
        for (int j = 0; j < ARRAY_SIZE(ctx->signals); j++) {
            if (!ctx->signals[j].sem.sem) {
                sig_ctx = &ctx->signals[j];
                break;
            }
        }
        if (!sig_ctx) {
            continue;
        }
        *sig_ctx = (signal_context_t){0};

        // Check if we're monitoring this fd for reading
        if (readfds && FD_ISSET(i, readfds)) {
            // Check if read buffer has data available
            if (ctx->read_buffer) {
                size_t available = xMessageBufferSpacesAvailable(ctx->read_buffer);
                // If buffer is not full, it means there's data to read
                if (available < READ_BUFFER_SIZE) {
                    fd_ready = true;
                } else {
                    FD_CLR(i, readfds);
                    sig_ctx->sem = signal_sem;
                    sig_ctx->read_fds = readfds;
                }
            }

            // Also check if channel is closed/eof
            if (!ssh_channel_is_open(ctx->channel) || ssh_channel_is_eof(ctx->channel)) {
                fd_ready = true;
            }
        }

        // Check if we're monitoring this fd for writing
        if (writefds && FD_ISSET(i, writefds)) {
            // Check if write buffer has space available
            if (ctx->write_buffer) {
                size_t available = xMessageBufferSpacesAvailable(ctx->write_buffer);
                // If buffer has space, writing is possible
                if (available > 0) {
                    fd_ready = true;
                } else {
                    sig_ctx->sem = signal_sem;
                    sig_ctx->write_fds = writefds;
                }
            }

            // If channel is closed, mark as ready (write will fail with error)
            if (!ssh_channel_is_open(ctx->channel)) {
                fd_ready = true;
            }
        }

        if (exceptfds && FD_ISSET(i, exceptfds)) {
            if (!ssh_channel_is_open(ctx->channel)) {
                fd_ready = true;
            }
        }

        // Signal if any fd is ready
        if (fd_ready) {
            esp_vfs_select_triggered(signal_sem);
            break;
        }
    }

    // We don't need to store anything for end_select in this implementation
    *end_select_args = NULL;

    return ESP_OK;
}

/**
 * @brief End select operation for SSH channels
 *
 * This function is called when select() completes or is cancelled.
 * It cleans up any resources allocated during start_select.
 *
 * @param end_select_args Context from start_select
 * @return ESP_OK on success
 */
static esp_err_t ssh_vfs_end_select(void *end_select_args)
{
    for (int i = 0; i < ARRAY_SIZE(channels); i++) {
        ssh_vfs_context_t *ctx = &channels[i];
        if (ctx->channel == NULL) {
            continue;
        }

        for (int j = 0; j < ARRAY_SIZE(ctx->signals); j++) {
            signal_context_t *sig_ctx = &ctx->signals[j];
            if (!sig_ctx->sem.sem) {
                continue;
            }
            bool clear = false;

            // Check if we're monitoring this fd for reading
            if (sig_ctx->read_fds && FD_ISSET(i, sig_ctx->read_fds)) {
                clear = true;
                // Check if read buffer has data available
                if (channels[i].read_buffer) {
                    size_t available = xMessageBufferSpacesAvailable(channels[i].read_buffer);
                    // If buffer is not full, it means there's data to read
                    if (available >= READ_BUFFER_SIZE) {
                        FD_CLR(i, sig_ctx->read_fds);
                    }
                }

                // Also check if channel is closed/eof
                if (ssh_channel_is_open(channels[i].channel) || ssh_channel_is_eof(channels[i].channel)) {
                    FD_CLR(i, sig_ctx->read_fds);
                }
            }

            // Check if we're monitoring this fd for writing
            if (sig_ctx->write_fds && FD_ISSET(i, sig_ctx->write_fds)) {
                clear = true;
                // Check if write buffer has space available
                if (channels[i].write_buffer) {
                    size_t available = xMessageBufferSpacesAvailable(channels[i].write_buffer);
                    // If buffer has space, writing is possible
                    if (available == 0) {
                        FD_CLR(i, sig_ctx->write_fds);
                    }
                }

                // If channel is closed, mark as ready (write will fail with error)
                if (ssh_channel_is_open(channels[i].channel)) {
                    FD_CLR(i, sig_ctx->write_fds);
                }
            }

            if (sig_ctx->error_fds && FD_ISSET(i, sig_ctx->error_fds)) {
                clear = true;
                if (ssh_channel_is_open(channels[i].channel)) {
                    FD_CLR(i, sig_ctx->error_fds);
                }
            }

            if (clear) {
                *sig_ctx = (signal_context_t){0};
            }
        }
    }

    // Nothing to clean up in our implementation
    return ESP_OK;
}
#endif

/**
 * @brief VFS read function for SSH channels
 *
 * This function is called when data is read from a file descriptor
 * that corresponds to an SSH channel (via the VFS layer).
 * It reads data from the message buffer that's fed by the SSH event loop.
 * This provides thread-safe access to SSH data.
 *
 * @param fd File descriptor (index into channels array)
 * @param data Buffer to store read data
 * @param size Maximum number of bytes to read
 * @return Number of bytes read, or -1 on error
 */
static ssize_t ssh_vfs_read(int fd, void *data, size_t size)
{
    size_t ch_idx = VFS_FD_TO_CHANNEL_INDEX(fd);
    if (ch_idx >= ARRAY_SIZE(channels) || channels[ch_idx].channel == NULL || channels[ch_idx].read_buffer == NULL) {
        errno = EBADF;
        ESP_LOGE(TAG, "Invalid file descriptor %d", fd);
        return -1;
    }

    // Block until data is available (with timeout)
    size_t bytes_received = xMessageBufferReceive(channels[ch_idx].read_buffer, data, size,
                                                  portMAX_DELAY // 1 second timeout
    );

    if (bytes_received == 0) {
        // Timeout - check if channel is still open
        if (!ssh_channel_is_open(channels[ch_idx].channel)) {
            errno = EPIPE;
            return -1;
        }
        errno = EAGAIN;
        return -1;
    }

    for (size_t i = 0; i < bytes_received; i++) {
        if (((char *)data)[i] == '\r') {
            ((char *)data)[i] = '\n';
        }
    }

    ESP_LOGD(TAG, "VFS read %zu bytes from message buffer", bytes_received);
    return bytes_received;
}

/**
 * @brief VFS write function for SSH channels
 *
 * This function is called when data is written to a file descriptor
 * that corresponds to an SSH channel (via the VFS layer).
 * It translates LF (\n) to CRLF (\r\n) for proper terminal output,
 * then puts data into a message buffer for the SSH event loop to consume.
 *
 * @param fd File descriptor (index into channels array)
 * @param data Buffer containing data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, or -1 on error
 */
static ssize_t ssh_vfs_write(int fd, const void *data, size_t size)
{
    size_t ch_idx = VFS_FD_TO_CHANNEL_INDEX(fd);
    if (ch_idx >= ARRAY_SIZE(channels) || channels[ch_idx].channel == NULL || channels[ch_idx].write_buffer == NULL) {
        errno = EBADF;
        ESP_LOGE(TAG, "Invalid file descriptor %d", fd);
        return -1;
    }

    ssh_channel channel = channels[ch_idx].channel;

    // Check if channel is still open
    if (!ssh_channel_is_open(channel)) {
        ESP_LOGE(TAG, "Attempt to write to closed channel");
        errno = EPIPE;
        return -1;
    }

    // Check if channel has reached EOF
    if (ssh_channel_is_eof(channel)) {
        ESP_LOGE(TAG, "Attempt to write to channel at EOF");
        errno = EPIPE;
        return -1;
    }

    // Count how many \n characters we have (to calculate buffer size needed)
    size_t lf_count = 0;
    const char *src = (const char *)data;
    for (size_t i = 0; i < size; i++) {
        if (src[i] == '\n') {
            lf_count++;
        }
    }

    char *data_to_send = (char *)data;
    size_t size_to_send = size;

    // Check if we need to translate line endings
    if (lf_count > 0) {

        // Allocate buffer for translated data (original size + extra bytes for \r)
        // Maximum expansion: each \n becomes \r\n (adds 1 byte per LF)
        size_t translated_size = size + lf_count;
        char *translated_data = malloc(translated_size);
        if (!translated_data) {
            ESP_LOGE(TAG, "Failed to allocate buffer for line ending translation");
            errno = ENOMEM;
            return -1;
        }

        // Translate \n to \r\n
        size_t dst_idx = 0;
        for (size_t i = 0; i < size; i++) {
            if (src[i] == '\n') {
                // Insert \r before \n
                translated_data[dst_idx++] = '\r';
                translated_data[dst_idx++] = '\n';
            } else {
                translated_data[dst_idx++] = src[i];
            }
        }

        ESP_LOGD(TAG, "Sent %zu bytes (translated from %zu) to write_buffer index %d", translated_size, size, ch_idx);
        data_to_send = translated_data;
        size_to_send = dst_idx;
    }

    // Send translated data to write buffer (blocking with max delay)
    size_t bytes_sent = xMessageBufferSend(channels[ch_idx].write_buffer, data_to_send, size_to_send, portMAX_DELAY);

    // If we allocated a new buffer, free it
    if (data_to_send != data) {
        free(data_to_send);
    }

    if (bytes_sent != size_to_send) {
        // Buffer is full - this is a flow control issue
        ESP_LOGW(TAG, "Write buffer full, sent %zu/%zu bytes", bytes_sent, size_to_send);
        if (bytes_sent == 0) {
            errno = EAGAIN;
            return -1;
        }
    }

    // Wake up the SSH event loop to process the write immediately
    if (wakeup_eventfd >= 0) {
        uint64_t signal = 1;
        write(wakeup_eventfd, &signal, sizeof(signal));
    }

    return size;
}

/**
 * @brief VFS close function for SSH channels
 *
 * This function is called when a file descriptor corresponding to
 * an SSH channel is closed. It properly closes the SSH channel
 * and cleans up resources including both message buffers.
 *
 * @param fd File descriptor (index into channels array)
 * @return 0 on success, -1 on error
 */
static int ssh_vfs_close(int fd)
{
    size_t ch_idx = VFS_FD_TO_CHANNEL_INDEX(fd);
    if (ch_idx >= ARRAY_SIZE(channels) || channels[ch_idx].channel == NULL) {
        errno = EBADF;
        ESP_LOGE(TAG, "Invalid file descriptor %d", fd);
        return -1;
    }

    ssh_channel channel = channels[ch_idx].channel;

    // Clean up message buffers
    if (channels[ch_idx].read_buffer) {
        vMessageBufferDelete(channels[ch_idx].read_buffer);
        channels[ch_idx].read_buffer = NULL;
    }
    if (channels[ch_idx].write_buffer) {
        vMessageBufferDelete(channels[ch_idx].write_buffer);
        channels[ch_idx].write_buffer = NULL;
    }

    // Clean up SSH channel
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    channels[ch_idx].channel = NULL;

    return 0;
}

/**
 * @brief VFS fcntl function for SSH channels
 *
 * This function handles file control operations (fcntl) for SSH channels.
 * Currently it's a stub that returns 0 (success) for all operations.
 *
 * @param fd File descriptor
 * @param cmd Command to execute
 * @param flags Flags for the command
 * @return 0 (always successful in this implementation)
 */
static int ssh_vfs_fcntl(int fd, int cmd, int flags)
{
    size_t ch_idx = VFS_FD_TO_CHANNEL_INDEX(fd);
    if (ch_idx >= ARRAY_SIZE(channels) || channels[ch_idx].channel == NULL) {
        errno = EBADF;
        ESP_LOGE(TAG, "Invalid file descriptor %d", fd);
        return -1;
    }

    ESP_LOGD(TAG, "ssh_vfs_fcntl called with fd=%d, cmd=%d, flags=%d", fd, cmd, flags);

    switch (cmd) {
    case F_GETFL:
        // Return the file access mode and status flags
        return VFS_FD_IS_WRITE(fd) ? O_WRONLY : O_RDONLY;

    case F_SETFL:
        // We don't support changing flags (non-blocking, etc.)
        // Just return success
        return 0;

    default:
        errno = EINVAL;
        return -1;
    }
}

static const esp_vfs_t vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .write = &ssh_vfs_write,
    .close = &ssh_vfs_close,
    .read = &ssh_vfs_read,
    .fcntl = &ssh_vfs_fcntl,
#ifdef CONFIG_VFS_SUPPORT_SELECT
    .start_select = &ssh_vfs_start_select,
    .end_select = &ssh_vfs_end_select,
#endif
};

/**
 * @brief SSH authentication callback for "none" method
 *
 * This callback is invoked when an SSH client attempts to authenticate
 * using the "none" method (no authentication). This is typically used
 * to discover what authentication methods are available.
 *
 * The function denies the "none" authentication and sets the available
 * authentication methods for the client to try.
 *
 * @param session The SSH session
 * @param user Username provided by the client
 * @param userdata User-defined data (not used here)
 * @return SSH_AUTH_DENIED (always denies "none" authentication)
 */
static int auth_none(ssh_session session, const char *user, void *userdata)
{
    ssh_server_config_t *config = (ssh_server_config_t *)userdata;
    ESP_LOGD(TAG, "Auth none requested for user: %s", user);

    // Authentication methods
    int methods = 0;
#ifdef CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
    if (config->password != NULL && strlen(config->password) > 0) {
        methods |= SSH_AUTH_METHOD_PASSWORD;
    }
#endif
#ifdef CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
    if (config->allowed_pubkeys != NULL && strlen(config->allowed_pubkeys) > 0) {
        methods |= SSH_AUTH_METHOD_PUBLICKEY;
    }
#endif

    ssh_set_auth_methods(session, methods);
    return SSH_AUTH_DENIED;
}

#if CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
/**
 * @brief SSH password authentication callback
 *
 * This callback is invoked when an SSH client attempts to authenticate
 * using username and password. It validates the provided credentials
 * against the configured default username and password.
 *
 * The function implements basic security by:
 * - Limiting authentication attempts to 3 tries
 * - Disconnecting the client after too many failed attempts
 * - Setting the global 'authenticated' flag on success
 *
 * @param session The SSH session
 * @param user Username provided by the client
 * @param password Password provided by the client
 * @param userdata User-defined data (not used here)
 * @return SSH_AUTH_SUCCESS if credentials are valid, SSH_AUTH_DENIED otherwise
 */
static int auth_password(ssh_session session, const char *user, const char *password, void *userdata)
{
    static int tries = 0;
    ssh_server_config_t *config = (ssh_server_config_t *)userdata;

    ESP_LOGD(TAG, "Password auth attempt for user: %s", user);

    if (strcmp(user, config->username) == 0 && strcmp(password, config->password) == 0) {
        ESP_LOGD(TAG, "Authentication successful for user: %s", user);
        tries = 0;                     // Reset tries on success
        last_auth_method = "password"; // Track successful auth method
        return SSH_AUTH_SUCCESS;
    }

    if (tries++ >= 3) {
        ESP_LOGD(TAG, "Too many authentication attempts");
        ssh_disconnect(session);
        return SSH_AUTH_DENIED;
    }

    ESP_LOGD(TAG, "Authentication failed (attempt %d/3)", tries);
    return SSH_AUTH_DENIED;
}
#endif // CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH

#if CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
/* Public key authentication using in-memory authorized_keys list */
static int auth_publickey(ssh_session session, const char *user, struct ssh_key_struct *pubkey, char signature_state, void *userdata)
{
    ssh_server_config_t *config = (ssh_server_config_t *)userdata;

    if (user == NULL || strcmp(user, config->username) != 0) {
        return SSH_AUTH_DENIED;
    }
    ESP_LOGI("DEBUG", "Public key authentication requested for user: %s", user);

    /* If client is probing supported keys (no signature), accept match to prompt
     * signature */
    const char *cursor = (const char *)config->allowed_pubkeys;
    while (cursor != NULL && *cursor != '\0') {
        const char *line_start = cursor;
        const char *nl = strchr(cursor, '\n');
        size_t line_len = (nl != NULL) ? (size_t)(nl - line_start) : strlen(line_start);

        /* Advance cursor for next iteration now to simplify continues */
        cursor = (nl != NULL) ? nl + 1 : line_start + line_len;

        /* Skip empty/whitespace-only lines */
        size_t leading_ws = 0U;
        while (leading_ws < line_len && isspace((unsigned char)line_start[leading_ws])) {
            leading_ws++;
        }
        if (leading_ws >= line_len) {
            continue;
        }

        /* Make a NUL-terminated copy of the current line */
        char *line = (char *)malloc(line_len + 1);
        if (line == NULL) {
            ESP_LOGI("DEBUG", "malloc failed at %d", __LINE__);
            break;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        /* Find end of type token (first whitespace) */
        const char *sp1 = line;
        while (*sp1 != '\0' && !isspace((unsigned char)*sp1)) {
            sp1++;
        }
        if (*sp1 == '\0') {
            free(line);
            continue;
        }
        size_t type_len = (size_t)(sp1 - line);
        if (type_len == 0) {
            free(line);
            continue;
        }

        char type_name[32];
        if (type_len >= sizeof(type_name)) {
            free(line);
            continue;
        }
        memcpy(type_name, line, type_len);
        type_name[type_len] = '\0';

        /* Skip whitespace to start of base64 */
        const char *b64_start = sp1;
        while (*b64_start != '\0' && isspace((unsigned char)*b64_start)) {
            b64_start++;
        }
        if (*b64_start == '\0' || *b64_start == '\n' || *b64_start == '\r') {
            free(line);
            continue;
        }
        /* Find end of base64 (next whitespace or end) */
        const char *p = b64_start;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        size_t b64_len = (size_t)(p - b64_start);
        if (b64_len == 0) {
            free(line);
            continue;
        }

        enum ssh_keytypes_e key_type = ssh_key_type_from_name(type_name);
        if (key_type == SSH_KEYTYPE_UNKNOWN) {
            free(line);
            continue;
        }

        /* Copy only the base64 blob (exclude trailing comment) */
        char *b64_copy = (char *)malloc(b64_len + 1);
        if (b64_copy == NULL) {
            ESP_LOGI("DEBUG", "malloc failed at %d", __LINE__);
            free(line);
            continue;
        }
        memcpy(b64_copy, b64_start, b64_len);
        b64_copy[b64_len] = '\0';

        ssh_key authorized_key = NULL;
        int rc = ssh_pki_import_pubkey_base64(b64_copy, key_type, &authorized_key);
        free(b64_copy);
        if (rc != SSH_OK || authorized_key == NULL) {
            if (authorized_key != NULL) {
                ssh_key_free(authorized_key);
            }
            free(line);
            continue;
        }
        rc = ssh_key_cmp(authorized_key, pubkey, SSH_KEY_CMP_PUBLIC);
        ssh_key_free(authorized_key);
        if (rc == 0) {
            free(line);
            if (signature_state == SSH_PUBLICKEY_STATE_NONE) {
                return SSH_AUTH_SUCCESS; /* tell client to sign */
            }

            if (signature_state == SSH_PUBLICKEY_STATE_VALID) {
                ESP_LOGI("DEBUG", "Public key authentication successful for user: %s", user);
                last_auth_method = "publickey"; // Track successful auth method
                return SSH_AUTH_SUCCESS;
            }

            return SSH_AUTH_DENIED;
        }

        free(line);
    }

    return SSH_AUTH_DENIED;
}
#endif // CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH

static void vfs_channel_close(ssh_session session, ssh_channel channel, void *userdata)
{
    ssh_vfs_context_t *ctx = (ssh_vfs_context_t *)userdata;
    ESP_LOGI(TAG, "Channel close requested");

    // Trigger select to wake up any pending operations
    trigger_select_for_channel(ctx->stdin_fd, false, false, true);
    trigger_select_for_channel(ctx->stdout_fd, false, false, true);

    // Close the VFS fds
    if (ctx->config->shell_task_kill_on_disconnect && ctx->shell_task_handle && ctx->shell_task_handle != xTaskGetCurrentTaskHandle()) {
        vTaskDelete(ctx->shell_task_handle);
        ctx->shell_task_handle = NULL;
    }
    if (ctx->stdin_fd >= 0) {
        close(ctx->stdin_fd);
        esp_vfs_unregister_fd(s_pipe_vfs_id, ctx->stdin_fd);
        ctx->stdin_fd = -1;
    }
    if (ctx->stdout_fd >= 0) {
        close(ctx->stdout_fd);
        esp_vfs_unregister_fd(s_pipe_vfs_id, ctx->stdout_fd);
        ctx->stdout_fd = -1;
    }
    if (ctx->read_buffer) {
        vMessageBufferDelete(ctx->read_buffer);
        ctx->read_buffer = NULL;
    }
    if (ctx->write_buffer) {
        vMessageBufferDelete(ctx->write_buffer);
        ctx->write_buffer = NULL;
    }
    if (ctx->channel_cb) {
        free(ctx->channel_cb);
        ctx->channel_cb = NULL;
    }

    // Do we need to clean up SSH channel?
    // if (ctx->channel) {
    //    ssh_channel_send_eof(ctx->channel);
    //    ssh_channel_close(ctx->channel);
    //    ssh_channel_free(ctx->channel);
    //    ctx->channel = NULL;
    //}

    // Free the channel context
    memset(ctx, 0, sizeof(ssh_vfs_context_t));
}

/**
 * @brief SSH channel open callback
 *
 * This callback is invoked when an SSH client requests to open a new channel
 * for a session. A channel is a logical connection within the SSH session
 * that carries the actual data (commands, shell I/O, etc.).
 *
 * The function:
 * - Finds a free slot in the channels array
 * - Creates a new SSH channel
 * - Sets up VFS integration by registering a file descriptor
 * - Associates the channel with callbacks for PTY and shell requests
 *
 * @param session The SSH session
 * @param userdata User-defined data (not used here)
 * @return The newly created SSH channel, or NULL if channel creation failed
 */
static ssh_channel channel_open(ssh_session session, void *userdata)
{
    ssh_server_config_t *config = (ssh_server_config_t *)userdata;

    ssh_vfs_context_t *ctx = allocate_new_channel_context();
    if (ctx == NULL) {
        ESP_LOGD(TAG, "No free channel found");
        return NULL;
    }

    ESP_LOGD(TAG, "Opening new channel");
    ctx->config = config;
    ctx->channel = ssh_channel_new(session);
    if (!ctx->channel) {
        ESP_LOGD(TAG, "Failed to create new channel");
        return NULL;
    }

    // Populate session information for the shell function
    populate_session_info(&ctx->session, session, config);

    // Create message buffer for this channel (4KB buffer)
    ctx->read_buffer = xMessageBufferCreate(READ_BUFFER_SIZE);
    if (!ctx->read_buffer) {
        ESP_LOGD(TAG, "Failed to create read message buffer");
        ssh_channel_free(ctx->channel);
        ctx->channel = NULL;
        return NULL;
    }

    // Create write message buffer for this channel (4KB buffer)
    ctx->write_buffer = xMessageBufferCreate(WRITE_BUFFER_SIZE);
    if (!ctx->write_buffer) {
        ESP_LOGD(TAG, "Failed to create write message buffer");
        vMessageBufferDelete(ctx->read_buffer);
        ctx->read_buffer = NULL;
        ssh_channel_free(ctx->channel);
        ctx->channel = NULL;
        return NULL;
    }

    ESP_LOGD(TAG, "Channel %d session opened, setting callbacks", CHANNEL_INDEX_FROM_PTR(ctx));

    ctx->channel_cb = malloc(sizeof(struct ssh_channel_callbacks_struct));
    if (!ctx->channel_cb) {
        ESP_LOGD(TAG, "Failed to allocate memory for channel callbacks");
        ssh_channel_free(ctx->channel);
        ctx->channel = NULL;
        return NULL;
    }
    *ctx->channel_cb = (struct ssh_channel_callbacks_struct){
        .userdata = ctx,
        .channel_pty_request_function = pty_request,
        .channel_shell_request_function = shell_request,
        .channel_close_function = vfs_channel_close,
        .channel_data_function = channel_data, // Add data handler for message buffer
    };
    ssh_callbacks_init(ctx->channel_cb);
    ssh_set_channel_callbacks(ctx->channel, ctx->channel_cb);

    ESP_LOGD(TAG, "Channel %d created", CHANNEL_INDEX_FROM_PTR(ctx));

    return ctx->channel;
}

static int ssh_event_fd_wrapper_callback(socket_t fd, int revents, void *userdata)
{
    (void)fd;
    (void)userdata;

    if (revents & POLLIN) {
        uint64_t value;
        read(wakeup_eventfd, &value, sizeof(value));
        ESP_LOGD(TAG, "Woke up SSH event loop from eventfd, value=%llu", value);
        // Drain write buffers after processing SSH events (same thread context!)
        for (uint32_t i = 0; i < value; i++) {
            drain_write_buffers();
        }
    }

    return SSH_OK;
}

static void ssh_server_internal(ssh_server_config_t *config)
{

    ssh_bind sshbind;
    ssh_session session;
    ssh_event event;
    int rc;

    // Wait a bit more to ensure network stack is fully ready
    ESP_LOGD(TAG, "SSH Server starting, waiting for network stack...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGD(TAG, "Network wait complete, initializing SSH server...");

    // Initialize libssh
    rc = ssh_init();
    if (rc != SSH_OK) {
        ESP_LOGE(TAG, "Failed to initialize libssh: %d", rc);
        return;
    }

    esp_err_t res = esp_vfs_register_with_id(&vfs, NULL, &s_pipe_vfs_id);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register VFS: %d", res);
        return;
    }

    // Create eventfd for waking up the SSH event loop when data is written
    wakeup_eventfd = eventfd(0, 0);
    if (wakeup_eventfd == -1) {
        ESP_LOGE(TAG, "Failed to create eventfd: %d", errno);
        esp_vfs_unregister_with_id(s_pipe_vfs_id);
        return;
    }
    ESP_LOGD(TAG, "Created wakeup eventfd: %d", wakeup_eventfd);

    // Create SSH bind object
    sshbind = ssh_bind_new();
    if (sshbind == NULL) {
        ESP_LOGE(TAG, "Failed to create SSH bind object");
        return;
    }

    // Set bind options
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDADDR, config->bindaddr);
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT_STR, config->port);
    if (config->debug_level) {
        ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_LOG_VERBOSITY_STR, config->debug_level);
    }

    rc = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, config->host_key);
    if (rc != SSH_OK) {
        ESP_LOGE(TAG, "Failed to set private key: %s", ssh_get_error(sshbind));
        ssh_bind_free(sshbind);
        return;
    }

    // Listen for connections
    rc = ssh_bind_listen(sshbind);
    if (rc != SSH_OK) {
        ESP_LOGE(TAG, "Failed to listen on %s:%s: %s", config->bindaddr, config->port, ssh_get_error(sshbind));
        ESP_LOGE(TAG, "This could indicate that the network stack is not ready yet.");
        ssh_bind_free(sshbind);
        return;
    }

    ESP_LOGD(TAG, "Simple SSH Server listening on %s:%s", config->bindaddr, config->port);
#if CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
    ESP_LOGD(TAG, "Default credentials: %s/%s", config->username, config->password);
#endif

    // Accept connections
    while (1) {
        session = ssh_new();
        if (session == NULL) {
            ESP_LOGE(TAG, "Failed to create session");
            continue;
        }

        rc = ssh_bind_accept(sshbind, session);
        if (rc != SSH_OK) {
            ESP_LOGE(TAG, "Failed to accept connection: %s", ssh_get_error(sshbind));
            ssh_free(session);
            continue;
        }

        ESP_LOGD(TAG, "New connection accepted");

        // Set up server callbacks
        struct ssh_server_callbacks_struct server_cb = {
            .userdata = config,
            .auth_none_function = auth_none,
#if CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH
            .auth_password_function = auth_password,
#endif
#if CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH
            .auth_pubkey_function = auth_publickey,
#endif
            .channel_open_request_session_function = channel_open,
        };

        ssh_callbacks_init(&server_cb);
        ssh_set_server_callbacks(session, &server_cb);
        ESP_LOGD(TAG, "Server callbacks set");

        // Handle key exchange
        // Note: ssh_handle_key_exchange() can return:
        // - SSH_OK: Key exchange completed successfully
        // - SSH_AGAIN: Key exchange in progress, need to call again
        // - SSH_ERROR: Fatal error occurred
        rc = ssh_handle_key_exchange(session);
        ESP_LOGD(TAG, "Key exchange result: rc=%d ", rc);
        if (rc == SSH_OK) {
            ESP_LOGD(TAG, "(SSH_OK - completed successfully)");
        } else if (rc == SSH_AGAIN) {
            ESP_LOGD(TAG, "(SSH_AGAIN - in progress)");
        } else if (rc == SSH_ERROR) {
            ESP_LOGD(TAG, "(SSH_ERROR - fatal error)");
        } else {
            ESP_LOGD(TAG, "(unknown return code %d)", rc);
        }

        if (rc == SSH_ERROR) {
            ESP_LOGE(TAG, "Key exchange failed: %s (bind: %s)", ssh_get_error(session), ssh_get_error(sshbind));
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        ESP_LOGD(TAG, "Key exchange completed or in progress");

        // Set up authentication methods
        auth_none(session, config->username, config);
        ESP_LOGD(TAG, "Authentication methods set");

        // Create event for session handling
        event = ssh_event_new();
        if (event == NULL) {
            ESP_LOGE(TAG, "Failed to create event");
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        // Add wakeup eventfd to the event loop
        if (ssh_event_add_fd(event, wakeup_eventfd, POLLIN, ssh_event_fd_wrapper_callback, NULL) != SSH_OK) {
            ESP_LOGE(TAG, "Failed to add wakeup eventfd to event");
            ssh_event_free(event);
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        // Add session to event
        if (ssh_event_add_session(event, session) != SSH_OK) {
            ESP_LOGE(TAG, "Failed to add session to event");
            ssh_event_free(event);
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        ESP_LOGD(TAG, "Session added to event, starting main loop");

        int poll_errors = 0;

        while (true) {

            int session_status = ssh_get_status(session);
            ESP_LOGD(TAG, "Session status: 0x%02x", session_status);
            if (session_status & SSH_CLOSED) {
                ESP_LOGD(TAG, "Session is closed");
                break;
            }
            if (session_status & SSH_CLOSED_ERROR) {
                ESP_LOGD(TAG, "Session is closed by error");
                break;
            }

            // Poll for SSH events (auth, channel requests, data, etc.)
            int poll_result = ssh_event_dopoll(event, 10000); // 10 second timeout

            if (poll_result == SSH_ERROR) {
                poll_errors++;
                ESP_LOGD(TAG, "Error polling events (count: %d): %s", poll_errors, ssh_get_error(session));

                // Allow a few poll errors before giving up
                if (poll_errors >= 10) {
                    ESP_LOGD(TAG, "Too many poll errors, terminating session");
                    break;
                }
            } else if (poll_result == SSH_OK) {
                // Reset error counter on successful poll
                poll_errors = 0;
            }
        }

        ssh_event_free(event);
        ssh_disconnect(session);
        ssh_free(session);
    }

    // Clean up
    if (wakeup_eventfd >= 0) {
        close(wakeup_eventfd);
        wakeup_eventfd = -1;
    }
    if (s_pipe_vfs_id != -1) {
        esp_vfs_unregister_with_id(s_pipe_vfs_id);
        s_pipe_vfs_id = -1;
    }
    ssh_bind_free(sshbind);
    ssh_finalize();
}

static bool ssh_run_server = false;

static void ssh_server(void *ctx)
{
    ssh_server_config_t *config = (ssh_server_config_t *)ctx;
    // Run the SSH server indefinitely
    while (ssh_run_server) {
        ssh_server_internal(config);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

esp_err_t ssh_server_start(ssh_server_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid SSH server configuration");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->port == NULL || config->bindaddr == NULL) {
        ESP_LOGE(TAG, "Invalid SSH server port or bind address, aborting");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->host_key == NULL) {
        ESP_LOGE(TAG, "No host key provided in configuration");
        return ESP_ERR_INVALID_ARG;
    }
    ssh_run_server = true;

    ESP_LOGI(TAG, "Starting SSH server...");
    BaseType_t ret = xTaskCreate(&ssh_server, "ssh_server", 8192, (void *)config, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SSH server task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ssh_server_stop()
{
    ESP_LOGI(TAG, "Stopping SSH server...");
    ssh_run_server = false;
}