| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- |

# SSH CLI Server Component

This ESP-IDF component provides a complete SSH server implementation with an integrated command-line interface. It allows remote access to your ESP32 device over SSH, providing the same console commands that are available over UART/USB.

## Why This Component?

While there are many SSH server implementations available, **this component is specifically designed to provide a true CLI experience** that forwards stdin/stdout just like your local ESP32 console. Unlike basic SSH servers that might only provide file transfer or simple command execution, this implementation:

- **Seamlessly integrates with ESP-IDF Console**: Access the exact same commands available on your local UART/USB console
- **True bidirectional I/O**: Real-time stdin/stdout forwarding with proper terminal emulation
- **Interactive shell experience**: Command history, tab completion, and line editing work exactly as expected
- **Session persistence**: Maintains state and context throughout the SSH session
- **Multiple concurrent sessions**: Each SSH connection gets its own isolated console environment

This bridges the gap between remote access and local development, giving you the same powerful console experience whether you're connected via USB cable or SSH over WiFi.

## Features

- **Full SSH Server**: Built on libssh library with support for key exchange, authentication, and encrypted communication
- **True Console Integration**: Complete stdin/stdout forwarding that works exactly like your local ESP32 console
- **Multiple Authentication Methods**:
  - Password authentication
  - Public key authentication
- **Interactive Shell**: Complete command-line interface with history, tab completion, and line editing
- **Session Tracking**: Real-time client information including IP, authentication method, and connection details
- **VFS Integration**: Thread-safe communication between SSH sessions and ESP console
- **Multi-channel Support**: Handle multiple concurrent SSH connections with isolated environments
- **Console Commands**: Access to all standard ESP-IDF console commands (WiFi, NVS, system info, etc.)
- **Built-in Session Commands**:
  - `session` - View current connection details
  - `exit`/`quit` - Graceful session termination


## Enhanced Shell Features

For an even more powerful shell experience with advanced features like pipes, grep, and other UNIX-style utilities, consider applying this ESP-IDF enhancement:

**ESP Shell Component Enhancement**: [ESP-IDF Pull Request #17793](https://github.com/espressif/esp-idf/pull/17793)

This patch extends the ESP-IDF console component to provide:
- **Pipe support**: Chain commands together (e.g., `help | grep wifi`)
- **Text filtering**: Use `grep` to filter command output
- **Enhanced command composition**: Build complex command pipelines
- **Improved shell scripting**: More sophisticated command interactions

When combined with this SSH server, you get a truly powerful remote shell environment that rivals traditional UNIX shells.

## Dependencies

This component requires:
- `david-cermak/libssh` - SSH library implementation for ESP32
- ESP-IDF v4.4 or later
- WiFi connectivity

## How to use this component

### 1. Add to your project

Add this component to your ESP-IDF project by including it in your `idf_component.yml`:

```yaml
dependencies:
  jimmyw/ssh_cli_server: "*"
```

Or clone this repository into your project's `components` directory.

### 2. Include in your application

```c
#include "ssh_server.h"

// Configure and start SSH server
ssh_server_config_t server_config = {
    .bindaddr = "0.0.0.0",
    .port = "22",
    .debug_level = "1",
    .username = "esp32",
    .host_key = host_key_pem_string,
    .password = "espressif",  // For password auth
    .shell_func = your_shell_function,
    .shell_func_ctx = shell_context,
    .shell_task_size = 8192,
    .shell_task_kill_on_disconnect = true,
};

esp_err_t err = ssh_server_start(&server_config);
```

### 3. Generate SSH Host Key

You need to provide an SSH host key in PEM format. Generate one using:

```bash
ssh-keygen -t ed25519 -f ssh_host_key -N ""
```

Then embed it in your firmware using `embed_files` in CMakeLists.txt or include it as a string.

### 4. Configure Authentication

The component supports multiple authentication methods via Kconfig options:

- `CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH` - Enable username/password authentication
- `CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH` - Enable public key authentication

## API Reference

### ssh_server_start()

Starts the SSH server with the provided configuration.

```c
esp_err_t ssh_server_start(ssh_server_config_t *config);
```

**Parameters:**
- `config`: Pointer to SSH server configuration structure

**Configuration Structure:**

```c
typedef struct {
    const char *bindaddr;              // IP address to bind (e.g., "0.0.0.0")
    const char *port;                  // Port to listen on (e.g., "22")
    const char *debug_level;           // SSH debug level ("0"-"3")
    const char *username;              // Username for authentication
    const char *host_key;              // SSH host private key (PEM format)
    const char *password;              // Password for password auth
    const char *allowed_pubkeys;       // Authorized public keys
    ssh_shell_func_t shell_func;       // Shell function to execute
    void *shell_func_ctx;              // Context for shell function
    uint32_t shell_task_size;          // Stack size for shell task
    bool shell_task_kill_on_disconnect; // Kill shell task on disconnect
} ssh_server_config_t;
```

## How to use the example

### 1. Configure WiFi

Set your WiFi credentials in menuconfig:
```
idf.py menuconfig
```

Navigate to `Example Configuration` and set:
- WiFi SSID
- WiFi Password

### 2. Build and Flash

```bash
cd examples/example_ssh_server
idf.py build flash monitor
```

### 3. Connect via SSH

Once the device is connected to WiFi, you can SSH to it:

```bash
ssh esp32@<device_ip_address>
# Password: espressif (default)
```

## Example Output

Device console output:
```
I (2000) example: Starting network connection...
I (5000) example: WiFi connected, IP: 192.168.1.100
I (5100) example: Starting SSH server...
I (5200) ssh_server: SSH Server listening on 0.0.0.0:22
I (5300) ssh_server: Default credentials: esp32/espressif
```

SSH client session:
```
$ ssh esp32@192.168.1.100
esp32@192.168.1.100's password:

=== SSH Session Information ===
Client: 192.168.1.50:54321
User: esp32
Auth method: password
Client version: SSH-2.0-OpenSSH_8.9
Session ID: 1
Connected at: 12345 seconds since boot
===============================

Welcome to ESP32 SSH Console
esp32> help
help     Print the list of registered commands
session  Show current SSH session information
exit     Exit the SSH session
quit     Exit the SSH session (alias for exit)
free     Get current free heap memory
esp32> free
Current Free Memory: 234560 bytes
esp32> wifi_info
WiFi Mode: STA
SSID: MyWiFi
IP Address: 192.168.1.100
esp32> session
Session ID: 1
Client: 192.168.1.50:54321
User: esp32
Auth method: password
Client version: SSH-2.0-OpenSSH_8.9
Connected: 12345 seconds since boot
esp32> exit
Connection to 192.168.1.100 closed.
```

## Built-in Commands

The SSH server includes several built-in commands in addition to all standard ESP-IDF console commands:

### Session Management Commands

- **`exit`** or **`quit`**: Gracefully closes the SSH session and returns to the client
- **`session`**: Displays detailed information about the current SSH session including:
  - Client IP address and port
  - Authenticated username
  - Authentication method used (password, publickey)
  - SSH client version string
  - Unique session ID
  - Connection timestamp

### Session Information Display

When you first connect via SSH, the server automatically displays session information showing who connected from where and how they authenticated. This provides immediate visibility into the connection details for security and debugging purposes.

## Troubleshooting

### SSH Connection Refused
- Verify WiFi is connected and device has IP address
- Check firewall settings on both device and client
- Ensure SSH server started successfully (check logs)

### Authentication Failed
- Verify username/password are correct (default: esp32/espressif)
- For public key auth, ensure the public key is properly formatted in authorized_keys format
- Check SSH debug logs by setting debug_level to "3"

### Performance Issues
- Increase shell_task_size if experiencing stack overflow
- Adjust message buffer sizes in ssh_server.c if needed
- Monitor free heap memory during operation

### Memory Issues
- SSH server uses approximately 20KB+ RAM per active connection
- Ensure sufficient free heap before starting SSH server
- Consider limiting concurrent connections for memory-constrained applications

### Build Issues
- Ensure ESP-IDF version is v4.4 or later
- Verify libssh component is properly installed
- Check that all required Kconfig options are enabled
