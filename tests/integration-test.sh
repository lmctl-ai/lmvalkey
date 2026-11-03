#!/usr/bin/env bash
# Integration test for lmvalkey: tests both Redis native protocol (RESP) and REST API.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Find binaries
SERVER_BIN="${REPO_ROOT}/src/lmvalkey"
if [[ ! -x "$SERVER_BIN" ]]; then
    SERVER_BIN="${REPO_ROOT}/src/valkey-server"
fi
CLI_BIN="${REPO_ROOT}/src/valkey-cli"

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "ERROR: Server binary not found at $SERVER_BIN" >&2
    exit 1
fi

if [[ ! -x "$CLI_BIN" ]]; then
    echo "ERROR: CLI binary not found at $CLI_BIN" >&2
    exit 1
fi

# Ports and workspace
RESP_PORT="${RESP_PORT:-26379}"
REST_PORT="${REST_PORT:-28080}"
TEST_DIR="$(mktemp -d -t lmvalkey-test-XXXXXX)"
PID_FILE="$TEST_DIR/valkey.pid"
LOG_FILE="$TEST_DIR/valkey.log"

cleanup() {
    local exit_code=$?
    echo "=== Cleaning up ==="
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid=$(cat "$PID_FILE" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            echo "Stopping server process $pid..."
            kill "$pid" 2>/dev/null || true
            for _ in {1..20}; do
                if ! kill -0 "$pid" 2>/dev/null; then break; fi
                sleep 0.1
            done
            kill -9 "$pid" 2>/dev/null || true
        fi
    fi
    fuser -k "${RESP_PORT}/tcp" "${REST_PORT}/tcp" 2>/dev/null || true
    if [[ $exit_code -ne 0 ]] && [[ -f "$LOG_FILE" ]]; then
        echo "=== Server Log on Failure ==="
        cat "$LOG_FILE"
    fi
    rm -rf "$TEST_DIR"
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

# Ensure test ports are clear before starting
fuser -k "${RESP_PORT}/tcp" "${REST_PORT}/tcp" 2>/dev/null || true
sleep 0.2

echo "=== Starting lmvalkey on RESP port $RESP_PORT, REST port $REST_PORT ==="
"$SERVER_BIN" \
    --port "$RESP_PORT" \
    --rest-port "$REST_PORT" \
    --daemonize yes \
    --pidfile "$PID_FILE" \
    --logfile "$LOG_FILE" \
    --dir "$TEST_DIR"

# Wait for readiness
echo "Waiting for server to become ready..."
READY=0
for i in {1..50}; do
    if "$CLI_BIN" -p "$RESP_PORT" ping >/dev/null 2>&1 && curl -s "http://127.0.0.1:$REST_PORT/ping" >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 0.1
done

if [[ $READY -ne 1 ]]; then
    echo "ERROR: Server failed to start within 5 seconds." >&2
    cat "$LOG_FILE"
    exit 1
fi
echo "Server is ready!"

echo "=========================================================="
echo "TEST SECTION 1: Redis Native Protocol (RESP)"
echo "=========================================================="

echo -n "Test 1.1: Native PING... "
RESP=$("$CLI_BIN" -p "$RESP_PORT" ping)
[[ "$RESP" == "PONG" ]] || { echo "FAIL (got $RESP)"; exit 1; }
echo "OK ($RESP)"

echo -n "Test 1.2: Native SET and GET... "
RESP=$("$CLI_BIN" -p "$RESP_PORT" set key_resp "hello_from_resp")
[[ "$RESP" == "OK" ]] || { echo "FAIL SET (got $RESP)"; exit 1; }
RESP=$("$CLI_BIN" -p "$RESP_PORT" get key_resp)
[[ "$RESP" == "hello_from_resp" ]] || { echo "FAIL GET (got $RESP)"; exit 1; }
echo "OK"

echo -n "Test 1.3: Native MSET and MGET... "
RESP=$("$CLI_BIN" -p "$RESP_PORT" mset mkey1 "val1" mkey2 "val2")
[[ "$RESP" == "OK" ]] || { echo "FAIL MSET (got $RESP)"; exit 1; }
RESP=$("$CLI_BIN" -p "$RESP_PORT" mget mkey1 mkey2)
echo "$RESP" | grep -q "val1" && echo "$RESP" | grep -q "val2" || { echo "FAIL MGET (got $RESP)"; exit 1; }
echo "OK"

echo -n "Test 1.4: Native List and Hash operations... "
"$CLI_BIN" -p "$RESP_PORT" lpush mylist "item1" "item2" >/dev/null
RESP=$("$CLI_BIN" -p "$RESP_PORT" lrange mylist 0 -1)
echo "$RESP" | grep -q "item2" || { echo "FAIL LPUSH/LRANGE (got $RESP)"; exit 1; }
"$CLI_BIN" -p "$RESP_PORT" hset myhash field1 "hval1" >/dev/null
RESP=$("$CLI_BIN" -p "$RESP_PORT" hget myhash field1)
[[ "$RESP" == "hval1" ]] || { echo "FAIL HGET (got $RESP)"; exit 1; }
echo "OK"

echo "=========================================================="
echo "TEST SECTION 2: REST HTTP Protocol"
echo "=========================================================="

echo -n "Test 2.1: REST /ping endpoint... "
REST_PING=$(curl -s "http://127.0.0.1:$REST_PORT/ping")
echo "$REST_PING" | grep -q '"ok":true' && echo "$REST_PING" | grep -q '"PONG"' || { echo "FAIL /ping (got $REST_PING)"; exit 1; }
echo "OK ($REST_PING)"

echo -n "Test 2.2: REST /set/<key> and /get/<key>... "
REST_SET=$(curl -s -X POST "http://127.0.0.1:$REST_PORT/set/key_rest" -d "hello_from_rest")
echo "$REST_SET" | grep -q '"ok":true' || { echo "FAIL POST /set (got $REST_SET)"; exit 1; }
REST_GET=$(curl -s "http://127.0.0.1:$REST_PORT/get/key_rest")
echo "$REST_GET" | grep -q '"ok":true' && echo "$REST_GET" | grep -q 'hello_from_rest' || { echo "FAIL GET /get (got $REST_GET)"; exit 1; }
echo "OK"

echo -n "Test 2.3: REST PUT /keys/<key>, GET /keys/<key>, and GET /keys... "
REST_PUT=$(curl -s -X PUT "http://127.0.0.1:$REST_PORT/keys/put_key" -d "put_value_data")
echo "$REST_PUT" | grep -q '"ok":true' || { echo "FAIL PUT /keys (got $REST_PUT)"; exit 1; }
REST_GET_KEY=$(curl -s "http://127.0.0.1:$REST_PORT/keys/put_key")
echo "$REST_GET_KEY" | grep -q 'put_value_data' || { echo "FAIL GET /keys/<key> (got $REST_GET_KEY)"; exit 1; }
REST_KEYS=$(curl -s "http://127.0.0.1:$REST_PORT/keys")
echo "$REST_KEYS" | grep -q 'put_key' || { echo "FAIL GET /keys (got $REST_KEYS)"; exit 1; }
echo "OK"

echo -n "Test 2.4: REST POST /cmd arbitrary command (JSON)... "
REST_CMD=$(curl -s -X POST "http://127.0.0.1:$REST_PORT/cmd" \
    -H "Content-Type: application/json" \
    -d '{"command":"MGET","args":["mkey1","mkey2"]}')
echo "$REST_CMD" | grep -q '"val1"' && echo "$REST_CMD" | grep -q '"val2"' || { echo "FAIL POST /cmd (got $REST_CMD)"; exit 1; }
echo "OK"

echo -n "Test 2.5: REST DELETE /keys/<key>... "
REST_DEL=$(curl -s -X DELETE "http://127.0.0.1:$REST_PORT/keys/put_key")
echo "$REST_DEL" | grep -q '"ok":true' || { echo "FAIL DELETE /keys (got $REST_DEL)"; exit 1; }
REST_GET_DEL=$(curl -s "http://127.0.0.1:$REST_PORT/get/put_key")
echo "$REST_GET_DEL" | grep -q 'null' || { echo "FAIL verify deleted key (got $REST_GET_DEL)"; exit 1; }
echo "OK"

echo "=========================================================="
echo "TEST SECTION 3: Cross-Protocol Interoperability"
echo "=========================================================="

echo -n "Test 3.1: Write via Native RESP -> Read via REST... "
"$CLI_BIN" -p "$RESP_PORT" set cross_resp_to_rest "resp_payload_123" >/dev/null
CROSS_REST=$(curl -s "http://127.0.0.1:$REST_PORT/get/cross_resp_to_rest")
echo "$CROSS_REST" | grep -q "resp_payload_123" || { echo "FAIL (got $CROSS_REST)"; exit 1; }
echo "OK"

echo -n "Test 3.2: Write via REST -> Read via Native RESP... "
curl -s -X POST "http://127.0.0.1:$REST_PORT/set/cross_rest_to_resp" -d "rest_payload_456" >/dev/null
CROSS_RESP=$("$CLI_BIN" -p "$RESP_PORT" get cross_rest_to_resp)
[[ "$CROSS_RESP" == "rest_payload_456" ]] || { echo "FAIL (got $CROSS_RESP)"; exit 1; }
echo "OK"

echo "=========================================================="
echo "TEST SECTION 4: Authentication Bypass for REST"
echo "=========================================================="

echo -n "Configuring server password on runtime... "
"$CLI_BIN" -p "$RESP_PORT" CONFIG SET requirepass "supersecretpass" >/dev/null
echo "OK"

echo -n "Test 4.1: Native RESP without auth MUST fail... "
RESP_NOAUTH_OUT=$("$CLI_BIN" -p "$RESP_PORT" ping 2>&1 || true)
echo "$RESP_NOAUTH_OUT" | grep -q "NOAUTH" || { echo "FAIL (expected NOAUTH, got $RESP_NOAUTH_OUT)"; exit 1; }
echo "OK (Rejected with NOAUTH as expected)"

echo -n "Test 4.2: Native RESP with auth succeeds... "
RESP_AUTH_OUT=$("$CLI_BIN" -p "$RESP_PORT" -a "supersecretpass" ping)
[[ "$RESP_AUTH_OUT" == "PONG" ]] || { echo "FAIL (got $RESP_AUTH_OUT)"; exit 1; }
echo "OK ($RESP_AUTH_OUT)"

echo -n "Test 4.3: REST endpoints succeed WITHOUT auth... "
REST_AUTH_BYPASS_PING=$(curl -s "http://127.0.0.1:$REST_PORT/ping")
echo "$REST_AUTH_BYPASS_PING" | grep -q '"ok":true' || { echo "FAIL REST /ping bypass (got $REST_AUTH_BYPASS_PING)"; exit 1; }
REST_AUTH_BYPASS_GET=$(curl -s "http://127.0.0.1:$REST_PORT/get/cross_resp_to_rest")
echo "$REST_AUTH_BYPASS_GET" | grep -q "resp_payload_123" || { echo "FAIL REST /get bypass (got $REST_AUTH_BYPASS_GET)"; exit 1; }
REST_AUTH_BYPASS_SET=$(curl -s -X POST "http://127.0.0.1:$REST_PORT/set/auth_free_key" -d "auth_free_val")
echo "$REST_AUTH_BYPASS_SET" | grep -q '"ok":true' || { echo "FAIL REST /set bypass (got $REST_AUTH_BYPASS_SET)"; exit 1; }
REST_AUTH_BYPASS_CMD=$(curl -s -X POST "http://127.0.0.1:$REST_PORT/cmd" \
    -H "Content-Type: application/json" \
    -d '{"command":"GET","args":["auth_free_key"]}')
echo "$REST_AUTH_BYPASS_CMD" | grep -q "auth_free_val" || { echo "FAIL REST /cmd bypass (got $REST_AUTH_BYPASS_CMD)"; exit 1; }
echo "OK"

# Reset requirepass
"$CLI_BIN" -p "$RESP_PORT" -a "supersecretpass" CONFIG SET requirepass "" >/dev/null

echo "=========================================================="
echo "TEST SECTION 5: lmconn Valkey Connector Client Tests"
echo "=========================================================="

LMCONN_PATH=""
if [[ -f "/home/ubuntu/lmconn/bin/lmconn.js" ]]; then
    LMCONN_PATH="/home/ubuntu/lmconn/bin/lmconn.js"
elif [[ -f "${REPO_ROOT}/../lmconn/bin/lmconn.js" ]]; then
    LMCONN_PATH="${REPO_ROOT}/../lmconn/bin/lmconn.js"
elif command -v lmconn >/dev/null 2>&1; then
    LMCONN_PATH="$(command -v lmconn)"
fi

if [[ -n "$LMCONN_PATH" ]] && command -v node >/dev/null 2>&1; then
    echo "Found lmconn at $LMCONN_PATH. Testing lmconn valkey connector..."

    echo -n "Test 5.1: lmconn valkey ping --mode resp... "
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey ping --port "$RESP_PORT" --mode resp)
    echo "$LMCONN_OUT" | grep -q '"ok":true' && echo "$LMCONN_OUT" | grep -q '"PONG"' || { echo "FAIL (got $LMCONN_OUT)"; exit 1; }
    echo "OK"

    echo -n "Test 5.2: lmconn valkey ping --mode rest... "
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey ping --port "$REST_PORT" --mode rest)
    echo "$LMCONN_OUT" | grep -q '"ok":true' && echo "$LMCONN_OUT" | grep -q '"PONG"' || { echo "FAIL (got $LMCONN_OUT)"; exit 1; }
    echo "OK"

    echo -n "Test 5.3: lmconn valkey set and get in RESP mode... "
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey set lmconn_resp_key "val_resp_123" --port "$RESP_PORT" --mode resp)
    echo "$LMCONN_OUT" | grep -q '"ok":true' || { echo "FAIL set resp (got $LMCONN_OUT)"; exit 1; }
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey get lmconn_resp_key --port "$RESP_PORT" --mode resp)
    echo "$LMCONN_OUT" | grep -q '"val_resp_123"' || { echo "FAIL get resp (got $LMCONN_OUT)"; exit 1; }
    echo "OK"

    echo -n "Test 5.4: lmconn valkey set and get in REST mode... "
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey set lmconn_rest_key "val_rest_456" --port "$REST_PORT" --mode rest)
    echo "$LMCONN_OUT" | grep -q '"ok":true' || { echo "FAIL set rest (got $LMCONN_OUT)"; exit 1; }
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey get lmconn_rest_key --port "$REST_PORT" --mode rest)
    echo "$LMCONN_OUT" | grep -q '"val_rest_456"' || { echo "FAIL get rest (got $LMCONN_OUT)"; exit 1; }
    echo "OK"

    echo -n "Test 5.5: lmconn valkey cross-protocol (set REST, get RESP)... "
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey get lmconn_rest_key --port "$RESP_PORT" --mode resp)
    echo "$LMCONN_OUT" | grep -q '"val_rest_456"' || { echo "FAIL cross get (got $LMCONN_OUT)"; exit 1; }
    echo "OK"

    echo -n "Test 5.6: lmconn valkey del and cmd... "
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey del lmconn_rest_key --port "$REST_PORT" --mode rest)
    echo "$LMCONN_OUT" | grep -q '"ok":true' || { echo "FAIL del rest (got $LMCONN_OUT)"; exit 1; }
    LMCONN_OUT=$(node "$LMCONN_PATH" valkey cmd ping "TEST_ARG" --port "$REST_PORT" --mode rest)
    echo "$LMCONN_OUT" | grep -q '"TEST_ARG"' || { echo "FAIL cmd rest (got $LMCONN_OUT)"; exit 1; }
    echo "OK"
else
    echo "lmconn not found; skipping lmconn wrapper tests (native and REST tests succeeded)."
fi

echo "=========================================================="
echo "TEST SECTION 6: lmcode Host and Batch Statements"
echo "=========================================================="

echo -n "Test 6.1: lmvalkey --version... "
VER_OUT=$("$SERVER_BIN" --version)
echo "$VER_OUT" | grep -q "lmvalkey 0.1.0" || { echo "FAIL (got $VER_OUT)"; exit 1; }
echo "OK ($VER_OUT)"

echo -n "Test 6.2: lmvalkey --lmcode-schema --json... "
SCHEMA_OUT=$("$SERVER_BIN" --lmcode-schema --json)
echo "$SCHEMA_OUT" | grep -q '"dsl.schema": 2' || { echo "FAIL (got $SCHEMA_OUT)"; exit 1; }
echo "$SCHEMA_OUT" | grep -q '"Valkey"' || { echo "FAIL (missing Valkey type)"; exit 1; }
echo "$SCHEMA_OUT" | grep -q '"Batch"' || { echo "FAIL (missing Batch type)"; exit 1; }
echo "OK"

echo -n "Test 6.3: lmvalkey --lmcode-signatures... "
SIG_OUT=$("$SERVER_BIN" --lmcode-signatures)
echo "$SIG_OUT" | grep -q 'context valkey: Valkey' || { echo "FAIL context (got $SIG_OUT)"; exit 1; }
echo "$SIG_OUT" | grep -q 'method Batch.exec() -> string' || { echo "FAIL method (got $SIG_OUT)"; exit 1; }
echo "OK"

if command -v lmscript >/dev/null 2>&1; then
    echo -n "Test 6.4: lmscript execution of batch statement via _lmcode_... "
    LMS_TMP="$TEST_DIR/test_batch.lms"
    cat << EOF > "$LMS_TMP"
s = load_schema(cli="$SERVER_BIN");
p = _lmcode_ s;
valkey.set("lmcode_batch_k1", "lmcode_batch_v1");
let b = valkey.batch();
b.set("lmcode_b2", "val2");
b.get("lmcode_batch_k1");
b.get("lmcode_b2");
let res = b.exec();
print("BATCH_RES:" + res);
end _lmcode_

r = run_lmcode(p);
require(r.ok, "host failed");
EOF
    LMS_OUT=$(VALKEY_PORT="$RESP_PORT" lmscript "$LMS_TMP")
    echo "$LMS_OUT" | grep -q 'BATCH_RES:\["OK","lmcode_batch_v1","val2"\]' || { echo "FAIL (got $LMS_OUT)"; exit 1; }
    # Verify key written to Valkey server
    VAL_CHECK=$("$CLI_BIN" -p "$RESP_PORT" get lmcode_b2)
    [[ "$VAL_CHECK" == "val2" ]] || { echo "FAIL server check (got $VAL_CHECK)"; exit 1; }
    echo "OK"
fi

echo "=========================================================="
echo "All integration tests passed successfully!"
echo "=========================================================="
