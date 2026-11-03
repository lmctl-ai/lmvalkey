# lmvalkey

[![Latest release](https://img.shields.io/github/v/release/lmctl-ai/lmvalkey?label=release)](https://github.com/lmctl-ai/lmvalkey/releases)
[![CI](https://github.com/lmctl-ai/lmvalkey/actions/workflows/ci.yml/badge.svg)](https://github.com/lmctl-ai/lmvalkey/actions/workflows/ci.yml)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](COPYING)

**lmvalkey** is a standalone, single-executable Valkey server featuring an **integrated lightweight REST web server** alongside the high-performance native Redis/Valkey (RESP) protocol engine.

- **Single Static Binary**: Self-contained executable with zero external runtime dependencies.
- **Dual-Protocol Engine**: Serves both native Redis protocol (RESP) clients and standard HTTP REST clients from a single unified datastore in memory.
- **Zero-Auth REST Interface**: HTTP clients access endpoints (`/get`, `/set`, `/keys`, `/cmd`) directly without authentication, even when native Valkey requires a password.
- **LMScript & lmcode Ready**: Seamlessly installed and invoked from LMScript and AI-agent environments.

---

## Installation

### Option 1: Automatic / Implicit with LMScript (Recommended)

When writing an LMScript (`.lms`) script, you do not need to install `lmvalkey` manually. Declaring `lmvalkey` in a `_lmcode_` block causes `lmscript` to automatically locate or fetch the appropriate pre-compiled binary for your system:

```lms
// Script example: my-task.lms
p = _lmcode_ cli="lmvalkey";
// lmscript automatically resolves and downloads the lmvalkey binary for your platform
end _lmcode_
r = run_lmcode(p);
```

### Option 2: Explicit via `lmscript install`

You can install `lmvalkey` directly using the LMScript package installer:

```bash
lmscript install github:lmctl-ai/lmvalkey
```

### Option 3: Direct Download from GitHub Releases

Pre-built, standalone executables for all major platforms are available on [GitHub Releases](https://github.com/lmctl-ai/lmvalkey/releases/tag/latest):

| Platform | Executable |
| :--- | :--- |
| **Linux (x86_64)** | `lmvalkey-linux-x64` |
| **Linux (ARM64)** | `lmvalkey-linux-arm64` |
| **macOS (Apple Silicon)** | `lmvalkey-macos-arm64` |
| **macOS (Intel)** | `lmvalkey-macos-x64` |
| **Windows (x86_64)** | `lmvalkey-windows-x64.exe` |

```bash
# Example: Download and run on Linux x86_64
curl -fsSL https://github.com/lmctl-ai/lmvalkey/releases/latest/download/lmvalkey-linux-x64 -o lmvalkey
chmod +x lmvalkey
```

---

## Starting the Server

Run `lmvalkey` specifying the native Redis/RESP port and the REST HTTP port:

```bash
# Listen on port 6379 for native RESP, and port 8080 for REST:
./lmvalkey --port 6379 --rest-port 8080

# Or run as a background daemon:
./lmvalkey --port 6379 --rest-port 8080 --daemonize yes
```

> **Note**: HTTP requests sent to the native port (6379) are also automatically detected and served.

---

## Unified In-Sync Data: REST (`curl`) & Native (`lmconn`)

`lmvalkey` maintains a single, unified in-memory database. Changes made through the REST HTTP interface are immediately visible on the native interface, and vice-versa.

Below are step-by-step examples using **`curl`** (for the REST interface) and **`lmconn`** (for the native RESP interface).

### Scenario A: Write via REST (`curl`) $\rightarrow$ Read via Native (`lmconn`)

1. **Write data via the REST interface with `curl`:**
   ```bash
   curl -X POST http://localhost:8080/set/user:42 -d '{"name":"Alice","role":"admin"}'
   ```
   *Response:*
   ```json
   {"ok":true,"result":"OK"}
   ```

2. **Read the exact same data via the Native RESP interface using `lmconn`:**
   ```bash
   lmconn valkey get user:42 --port 6379 --mode resp
   ```
   *Output:*
   ```json
   {
     "ok": true,
     "result": {
       "mode": "resp",
       "host": "127.0.0.1",
       "port": 6379,
       "result": "{\"name\":\"Alice\",\"role\":\"admin\"}"
     }
   }
   ```

---

### Scenario B: Write via Native (`lmconn`) $\rightarrow$ Read via REST (`curl`)

1. **Write data via the Native interface using `lmconn`:**
   ```bash
   lmconn valkey set session:xyz "active-user-token" --port 6379 --mode resp
   ```
   *Output:*
   ```json
   {
     "ok": true,
     "result": {
       "mode": "resp",
       "host": "127.0.0.1",
       "port": 6379,
       "result": "OK"
     }
   }
   ```

2. **Read the exact same key via the REST interface with `curl`:**
   ```bash
   curl http://localhost:8080/get/session:xyz
   ```
   *Response:*
   ```json
   {
     "ok": true,
     "result": "active-user-token"
   }
   ```

---

### Scenario C: Multi-key Commands and Sync

1. **Write multiple keys atomically via native command in `lmconn`:**
   ```bash
   lmconn valkey cmd MSET metrics:cpu "42%" metrics:mem "68%" --port 6379 --mode resp
   ```

2. **Retrieve all keys matching a pattern via REST:**
   ```bash
   curl http://localhost:8080/keys/metrics:*
   ```
   *Response:*
   ```json
   {
     "ok": true,
     "result": ["metrics:cpu", "metrics:mem"]
   }
   ```

3. **Execute arbitrary command (`MGET`) via REST JSON body:**
   ```bash
   curl -X POST http://localhost:8080/cmd \
     -H "Content-Type: application/json" \
     -d '{"command": "MGET", "args": ["metrics:cpu", "metrics:mem"]}'
   ```
   *Response:*
   ```json
   {
     "ok": true,
     "result": ["42%", "68%"]
   }
   ```

---

### Scenario D: Authentication Bypass for REST

When Valkey is configured with a password (`requirepass secret123`):

1. **Native RESP clients must authenticate:**
   ```bash
   lmconn valkey ping --port 6379 --mode resp
   # Error: (error) NOAUTH Authentication required.
   ```

2. **REST clients access the database directly without authentication:**
   ```bash
   curl http://localhost:8080/ping
   # Output: {"ok":true,"result":"PONG"}

   curl http://localhost:8080/get/user:42
   # Output: {"ok":true,"result":"{\"name\":\"Alice\",\"role\":\"admin\"}"}
   ```

---

## REST API Reference

All REST endpoints return JSON envelopes in the format:
```json
{"ok": true, "result": ...}
```
or on error:
```json
{"ok": false, "error": "Error description"}
```

| Method | Path | Description | Example |
| :--- | :--- | :--- | :--- |
| `GET` | `/` | Health check & server status | `curl http://localhost:8080/` |
| `GET` | `/ping` | Server ping | `curl http://localhost:8080/ping` |
| `GET` | `/get/<key>` | Get string value of key | `curl http://localhost:8080/get/mykey` |
| `POST` | `/set/<key>` | Set value of key via request body | `curl -X POST http://localhost:8080/set/mykey -d "value"` |
| `POST` | `/set/<key>/<val>` | Set value of key via path | `curl -X POST http://localhost:8080/set/mykey/value` |
| `GET` | `/keys` | List all keys (`KEYS *`) | `curl http://localhost:8080/keys` |
| `GET` | `/keys/<pattern>` | List keys matching pattern | `curl http://localhost:8080/keys/user:*` |
| `PUT` | `/keys/<key>` | RESTful set key value | `curl -X PUT http://localhost:8080/keys/item:1 -d '{"id":1}'` |
| `DELETE` | `/keys/<key>` | Delete a key | `curl -X DELETE http://localhost:8080/keys/item:1` |
| `POST` | `/cmd` | Execute arbitrary command | `curl -X POST http://localhost:8080/cmd -d '["INFO"]'` |

---

## Native Client Reference (`lmconn`)

`lmconn` provides a zero-dependency CLI client for Valkey supporting both RESP and REST modes:

```bash
# Ping server
lmconn valkey ping [--port 6379] [--mode resp|rest]

# Set a key
lmconn valkey set <key> <value> [--port 6379] [--mode resp|rest]

# Get a key
lmconn valkey get <key> [--port 6379] [--mode resp|rest]

# Delete a key
lmconn valkey del <key> [--port 6379] [--mode resp|rest]

# Run arbitrary command
lmconn valkey cmd <COMMAND> [args...] [--port 6379] [--mode resp|rest]
```

---

## LMScript & `lmcode` Batch Statements

`lmvalkey` embeds the `lmcode-cpp` runtime (DSLB v6), allowing LMScript (`.lms`) programs to execute type-checked bytecode directly inside or against `lmvalkey`, construct pipelined batch statements, and execute operations with minimal round-trip overhead.

### Batch Operations in `.lms`

```lms
// Using installed lmvalkey host
s = load_schema(cli="lmvalkey" install="github:lmctl-ai/lmvalkey@latest");
p = _lmcode_ s;
  // Direct key-value operations
  valkey.set("user:100", "Alice");
  let user = valkey.get("user:100");
  print("User: " + user);

  // Construct a batch statement
  let b = valkey.batch();
  b.set("item:1", "Apples");
  b.set("item:2", "Oranges");
  b.get("item:1");
  b.get("item:2");

  // Execute the entire batch in a single pipeline round-trip
  let results = b.exec();
  print("Batch results: " + results);
end _lmcode_

r = run_lmcode(p);
require(r.ok, "batch execution failed");
```

### Schema & Signatures

`lmvalkey` exposes its lmcode contract via standard host flags:
```bash
# Print compiler schema
lmvalkey --lmcode-schema --json

# Print canonical registry signatures
lmvalkey --lmcode-signatures
```

---

## License

`lmvalkey` is licensed under the [BSD-3-Clause](COPYING) license.
