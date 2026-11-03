#include "server.h"
#include "rest.h"
#include "version.h"
#include "cluster.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations */
void releaseReplyReferences(client *c);

/* Check if the incoming query buffer begins with an HTTP request. */
int isHttpRequest(const char *buf, size_t len) {
    if (len < 4) return 0;
    if (!strncmp(buf, "GET /", 5) ||
        !strncmp(buf, "POST ", 5) ||
        !strncmp(buf, "PUT ", 4) ||
        !strncmp(buf, "DELETE ", 7) ||
        !strncmp(buf, "OPTIONS ", 8) ||
        !strncmp(buf, "HEAD ", 5) ||
        !strncmp(buf, "PATCH ", 6)) {
        return 1;
    }
    if (!strncmp(buf, "GET ", 4)) {
        const char *crlf = memmem(buf, len, "\r\n", 2);
        size_t linelen = crlf ? (size_t)(crlf - buf) : len;
        if (memmem(buf, linelen, " HTTP/1.", 8) || memmem(buf, linelen, " HTTP/2", 7)) {
            return 1;
        }
    }
    return 0;
}

/* URL-decode a string into a new SDS string */
static sds urlDecode(const char *src, size_t len) {
    sds dst = sdsempty();
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
            char hex[3] = {src[i+1], src[i+2], 0};
            char val = (char)strtol(hex, NULL, 16);
            dst = sdscatlen(dst, &val, 1);
            i += 2;
        } else if (src[i] == '+') {
            dst = sdscatlen(dst, " ", 1);
        } else {
            dst = sdscatlen(dst, &src[i], 1);
        }
    }
    return dst;
}

/* Append a string as an escaped JSON string: "..." */
static sds sdscatJsonString(sds s, const char *p, size_t len) {
    s = sdscatlen(s, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
        case '\\': s = sdscatlen(s, "\\\\", 2); break;
        case '"':  s = sdscatlen(s, "\\\"", 2); break;
        case '\b': s = sdscatlen(s, "\\b", 2); break;
        case '\f': s = sdscatlen(s, "\\f", 2); break;
        case '\n': s = sdscatlen(s, "\\n", 2); break;
        case '\r': s = sdscatlen(s, "\\r", 2); break;
        case '\t': s = sdscatlen(s, "\\t", 2); break;
        default:
            if (c < 32) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                s = sdscat(s, buf);
            } else {
                s = sdscatlen(s, (char *)&c, 1);
            }
            break;
        }
    }
    s = sdscatlen(s, "\"", 1);
    return s;
}

/* Parse a single RESP value from *ptr up to end and serialize to JSON */
static sds parseRespValue(const char **ptr, const char *end, int *is_error) {
    if (*ptr >= end) return sdsnew("null");
    char type = **ptr;
    (*ptr)++;

    if (type == '+' || type == '-') {
        if (type == '-') *is_error = 1;
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        size_t len = crlf ? (size_t)(crlf - *ptr) : (size_t)(end - *ptr);
        sds res = sdscatJsonString(sdsempty(), *ptr, len);
        *ptr = crlf ? crlf + 2 : end;
        return res;
    } else if (type == ':') {
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        size_t len = crlf ? (size_t)(crlf - *ptr) : (size_t)(end - *ptr);
        sds res = sdsnewlen(*ptr, len);
        *ptr = crlf ? crlf + 2 : end;
        return res;
    } else if (type == '$') {
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        if (!crlf) { *ptr = end; return sdsnew("null"); }
        long long bulklen = strtoll(*ptr, NULL, 10);
        *ptr = crlf + 2;
        if (bulklen < 0) return sdsnew("null");
        if (*ptr + bulklen > end) {
            size_t avail = end - *ptr;
            sds res = sdscatJsonString(sdsempty(), *ptr, avail);
            *ptr = end;
            return res;
        }
        sds res = sdscatJsonString(sdsempty(), *ptr, bulklen);
        *ptr += bulklen;
        if (*ptr + 2 <= end && (*ptr)[0] == '\r' && (*ptr)[1] == '\n') {
            *ptr += 2;
        }
        return res;
    } else if (type == '*') {
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        if (!crlf) { *ptr = end; return sdsnew("null"); }
        long long count = strtoll(*ptr, NULL, 10);
        *ptr = crlf + 2;
        if (count < 0) return sdsnew("null");
        sds res = sdsnew("[");
        for (long long i = 0; i < count; i++) {
            if (*ptr >= end) break;
            if (i > 0) res = sdscatlen(res, ",", 1);
            sds elem = parseRespValue(ptr, end, is_error);
            res = sdscatsds(res, elem);
            sdsfree(elem);
        }
        res = sdscatlen(res, "]", 1);
        return res;
    } else if (type == '#') {
        char b = **ptr;
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        *ptr = crlf ? crlf + 2 : end;
        return sdsnew(b == 't' ? "true" : "false");
    } else if (type == ',') {
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        size_t len = crlf ? (size_t)(crlf - *ptr) : (size_t)(end - *ptr);
        sds res = sdsnewlen(*ptr, len);
        *ptr = crlf ? crlf + 2 : end;
        return res;
    } else if (type == '_') {
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        *ptr = crlf ? crlf + 2 : end;
        return sdsnew("null");
    } else if (type == '%' || type == '~') {
        const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
        if (!crlf) { *ptr = end; return sdsnew("null"); }
        long long count = strtoll(*ptr, NULL, 10);
        *ptr = crlf + 2;
        if (count < 0) return sdsnew("null");
        if (type == '%') {
            sds res = sdsnew("{");
            for (long long i = 0; i < count; i++) {
                if (*ptr >= end) break;
                if (i > 0) res = sdscatlen(res, ",", 1);
                sds key = parseRespValue(ptr, end, is_error);
                res = sdscatsds(res, key);
                res = sdscatlen(res, ":", 1);
                sdsfree(key);
                sds val = parseRespValue(ptr, end, is_error);
                res = sdscatsds(res, val);
                sdsfree(val);
            }
            res = sdscatlen(res, "}", 1);
            return res;
        } else {
            sds res = sdsnew("[");
            for (long long i = 0; i < count; i++) {
                if (*ptr >= end) break;
                if (i > 0) res = sdscatlen(res, ",", 1);
                sds elem = parseRespValue(ptr, end, is_error);
                res = sdscatsds(res, elem);
                sdsfree(elem);
            }
            res = sdscatlen(res, "]", 1);
            return res;
        }
    }
    const char *crlf = memmem(*ptr, end - *ptr, "\r\n", 2);
    size_t len = crlf ? (size_t)(crlf - *ptr) : (size_t)(end - *ptr);
    sds res = sdscatJsonString(sdsempty(), *ptr, len);
    *ptr = crlf ? crlf + 2 : end;
    return res;
}

/* Parse JSON array of strings: ["SET", "k", "v"] into SDS array */
static int parseJsonStringArray(const char *p, size_t len, sds **argv_out, int *argc_out) {
    const char *end = p + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '[') return 0;
    p++;

    int cap = 8;
    int count = 0;
    sds *args = zmalloc(sizeof(sds) * cap);

    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p == ']') {
            if (p < end && *p == ']') p++;
            break;
        }
        if (*p == ',') { p++; continue; }

        if (*p == '"') {
            p++;
            sds val = sdsempty();
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    p++;
                    if (*p == 'n') val = sdscatlen(val, "\n", 1);
                    else if (*p == 'r') val = sdscatlen(val, "\r", 1);
                    else if (*p == 't') val = sdscatlen(val, "\t", 1);
                    else if (*p == '\\') val = sdscatlen(val, "\\", 1);
                    else if (*p == '"') val = sdscatlen(val, "\"", 1);
                    else if (*p == '/') val = sdscatlen(val, "/", 1);
                    else val = sdscatlen(val, p, 1);
                } else {
                    val = sdscatlen(val, p, 1);
                }
                p++;
            }
            if (p < end && *p == '"') p++;
            if (count >= cap) {
                cap *= 2;
                args = zrealloc(args, sizeof(sds) * cap);
            }
            args[count++] = val;
        } else {
            const char *tok_start = p;
            while (p < end && *p != ',' && *p != ']' && !isspace((unsigned char)*p)) p++;
            sds val = sdsnewlen(tok_start, p - tok_start);
            if (count >= cap) {
                cap *= 2;
                args = zrealloc(args, sizeof(sds) * cap);
            }
            args[count++] = val;
        }
    }

    if (count == 0) {
        zfree(args);
        return 0;
    }
    *argv_out = args;
    *argc_out = count;
    return 1;
}

/* Parse JSON object {"command":"PING","args":["a","b"]} or {"cmd":"PING"} */
static int parseJsonObjectCommand(const char *p, size_t len, sds **argv_out, int *argc_out) {
    const char *end = p + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '{') return 0;

    /* Look for "command" or "cmd" */
    const char *cmd_key = memmem(p, end - p, "\"command\"", 9);
    if (!cmd_key) cmd_key = memmem(p, end - p, "\"cmd\"", 5);
    if (!cmd_key) return 0;

    const char *colon = memchr(cmd_key, ':', end - cmd_key);
    if (!colon) return 0;
    const char *val_start = colon + 1;
    while (val_start < end && isspace((unsigned char)*val_start)) val_start++;
    if (val_start >= end || *val_start != '"') return 0;
    val_start++;
    const char *val_end = memchr(val_start, '"', end - val_start);
    if (!val_end) return 0;

    sds cmd = sdsnewlen(val_start, val_end - val_start);
    int cap = 8;
    int count = 1;
    sds *args = zmalloc(sizeof(sds) * cap);
    args[0] = cmd;

    /* Look for "args" */
    const char *args_key = memmem(p, end - p, "\"args\"", 6);
    if (args_key) {
        const char *bracket = memchr(args_key, '[', end - args_key);
        if (bracket && bracket < end) {
            sds *sub_args = NULL;
            int sub_count = 0;
            if (parseJsonStringArray(bracket, end - bracket, &sub_args, &sub_count)) {
                if (count + sub_count > cap) {
                    cap = count + sub_count + 4;
                    args = zrealloc(args, sizeof(sds) * cap);
                }
                for (int i = 0; i < sub_count; i++) {
                    args[count++] = sub_args[i];
                }
                zfree(sub_args);
            }
        }
    }

    *argv_out = args;
    *argc_out = count;
    return 1;
}

/* Parse HTTP request buffer */
void parseRestBuffer(client *c) {
    char *buf = c->querybuf + c->qb_pos;
    size_t qblen = sdslen(c->querybuf) - c->qb_pos;

    /* Search for end of HTTP headers */
    const char *header_end = memmem(buf, qblen, "\r\n\r\n", 4);
    int header_sep_len = 4;
    if (!header_end) {
        header_end = memmem(buf, qblen, "\n\n", 2);
        header_sep_len = 2;
    }
    if (!header_end) {
        if (qblen > PROTO_INLINE_MAX_SIZE) {
            c->read_flags |= READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
        }
        return; /* Incomplete headers */
    }

    size_t headers_len = (header_end - buf) + header_sep_len;

    /* Parse Content-Length and Connection */
    long long content_length = 0;
    int keep_alive = 1;

    const char *cl = buf;
    while (cl < header_end) {
        if (!strncasecmp(cl, "Content-Length:", 15)) {
            cl += 15;
            while (cl < header_end && isspace((unsigned char)*cl)) cl++;
            content_length = strtoll(cl, NULL, 10);
            break;
        }
        const char *next_line = memmem(cl, header_end - cl, "\n", 1);
        if (!next_line) break;
        cl = next_line + 1;
    }

    const char *conn_hdr = buf;
    while (conn_hdr < header_end) {
        if (!strncasecmp(conn_hdr, "Connection:", 11)) {
            conn_hdr += 11;
            while (conn_hdr < header_end && isspace((unsigned char)*conn_hdr)) conn_hdr++;
            if (!strncasecmp(conn_hdr, "close", 5)) {
                keep_alive = 0;
            } else if (!strncasecmp(conn_hdr, "keep-alive", 10)) {
                keep_alive = 1;
            }
            break;
        }
        const char *next_line = memmem(conn_hdr, header_end - conn_hdr, "\n", 1);
        if (!next_line) break;
        conn_hdr = next_line + 1;
    }

    if (content_length < 0 || content_length > server.proto_max_bulk_len) {
        c->read_flags |= READ_FLAGS_ERROR_MBULK_INVALID_BULK_LEN;
        return;
    }

    size_t total_req_len = headers_len + (size_t)content_length;
    if (qblen < total_req_len) {
        return; /* Incomplete body */
    }

    const char *body = buf + headers_len;
    size_t body_len = (size_t)content_length;
    c->flag.rest_client = 1;
    c->flag.rest_keep_alive = keep_alive;

    /* Parse request line */
    char method[16] = {0};
    char uri[2048] = {0};
    char version[16] = {0};

    sscanf(buf, "%15s %2047s %15s", method, uri, version);
    if (version[0] && !strcasecmp(version, "HTTP/1.0") && !conn_hdr) {
        c->flag.rest_keep_alive = 0;
    }

    /* CORS preflight OPTIONS */
    if (!strcasecmp(method, "OPTIONS")) {
        sds reply = sdscatprintf(sdsempty(),
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, HEAD\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "Connection: %s\r\n\r\n",
            c->flag.rest_keep_alive ? "keep-alive" : "close");
        addReplyProto(c, reply, sdslen(reply));
        sdsfree(reply);
        if (!c->flag.rest_keep_alive) c->flag.close_after_reply = 1;
        c->qb_pos += total_req_len;
        c->qb_applied = c->qb_pos;
        c->net_input_bytes_curr_cmd = total_req_len;
        c->argc = 0;
        return;
    }

    /* Strip query string from URI */
    char path[2048];
    snprintf(path, sizeof(path), "%s", uri);
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    /* Root / health check endpoint */
    if (!strcasecmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/help") || !strcmp(path, "/health"))) {
        sds json = sdscatprintf(sdsempty(),
            "{\"ok\":true,\"result\":{\"server\":\"valkey\",\"version\":\"%s\",\"status\":\"ready\"}}\n",
            VALKEY_VERSION);
        sds resp = sdscatprintf(sdsempty(),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, HEAD\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Connection: %s\r\n\r\n%s",
            sdslen(json),
            c->flag.rest_keep_alive ? "keep-alive" : "close",
            json);
        addReplyProto(c, resp, sdslen(resp));
        sdsfree(resp);
        sdsfree(json);
        if (!c->flag.rest_keep_alive) c->flag.close_after_reply = 1;
        c->qb_pos += total_req_len;
        c->qb_applied = c->qb_pos;
        c->net_input_bytes_curr_cmd = total_req_len;
        c->argc = 0;
        return;
    }

    sds *args = NULL;
    int argc = 0;

    /* POST / or /cmd or /exec with JSON array, JSON object, or text body */
    if (body_len > 0 && (!strcmp(path, "/") || !strcmp(path, "/cmd") || !strcmp(path, "/exec"))) {
        if (!parseJsonStringArray(body, body_len, &args, &argc) &&
            !parseJsonObjectCommand(body, body_len, &args, &argc)) {
            args = sdsnsplitargs(body, body_len, &argc);
        }
    }

    /* RESTful KV endpoint: /keys/<key> or /key/<key> */
    if (!args && (!strncmp(path, "/keys/", 6) || !strncmp(path, "/key/", 5) || !strcmp(path, "/keys") || !strcmp(path, "/key"))) {
        const char *k = path + (!strncmp(path, "/keys", 5) ? 5 : 4);
        if (*k == '/') k++;
        if (*k == '\0') {
            argc = 2;
            args = zmalloc(sizeof(sds) * 2);
            args[0] = sdsnew("KEYS");
            args[1] = sdsnew("*");
        } else {
            sds key = urlDecode(k, strlen(k));
            if (!strcasecmp(method, "GET")) {
                argc = 2;
                args = zmalloc(sizeof(sds) * 2);
                args[0] = sdsnew("GET");
                args[1] = key;
            } else if (!strcasecmp(method, "POST") || !strcasecmp(method, "PUT")) {
                argc = 3;
                args = zmalloc(sizeof(sds) * 3);
                args[0] = sdsnew("SET");
                args[1] = key;
                args[2] = sdsnewlen(body, body_len);
            } else if (!strcasecmp(method, "DELETE")) {
                argc = 2;
                args = zmalloc(sizeof(sds) * 2);
                args[0] = sdsnew("DEL");
                args[1] = key;
            } else {
                sdsfree(key);
            }
        }
    }

    /* Path-based command: /<COMMAND>/<arg1>/<arg2>/... */
    if (!args) {
        char *pcopy = zstrdup(path);
        char *token, *saveptr;
        int cap = 8;
        args = zmalloc(sizeof(sds) * cap);
        argc = 0;

        for (token = strtok_r(pcopy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
            if (strlen(token) == 0) continue;
            if (argc >= cap) {
                cap *= 2;
                args = zrealloc(args, sizeof(sds) * cap);
            }
            args[argc++] = urlDecode(token, strlen(token));
        }
        zfree(pcopy);

        if (argc > 0) {
            /* If DELETE method and first token is not DEL or UNLINK: prepend DEL */
            if (!strcasecmp(method, "DELETE") && strcasecmp(args[0], "DEL") && strcasecmp(args[0], "UNLINK")) {
                if (argc + 1 > cap) {
                    cap += 2;
                    args = zrealloc(args, sizeof(sds) * cap);
                }
                for (int i = argc; i > 0; i--) args[i] = args[i - 1];
                args[0] = sdsnew("DEL");
                argc++;
            }
            /* If POST or PUT method and body_len > 0, append body as argument */
            if ((!strcasecmp(method, "POST") || !strcasecmp(method, "PUT")) && body_len > 0) {
                if (argc + 1 > cap) {
                    cap += 2;
                    args = zrealloc(args, sizeof(sds) * cap);
                }
                args[argc++] = sdsnewlen(body, body_len);
            }
        }
    }

    if (argc == 0) {
        sds json = sdsnew("{\"ok\":false,\"error\":\"ERR missing command or invalid path\"}\n");
        sds resp = sdscatprintf(sdsempty(),
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, HEAD\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Connection: %s\r\n\r\n%s",
            sdslen(json),
            c->flag.rest_keep_alive ? "keep-alive" : "close",
            json);
        addReplyProto(c, resp, sdslen(resp));
        sdsfree(resp);
        sdsfree(json);
        if (!c->flag.rest_keep_alive) c->flag.close_after_reply = 1;
        c->qb_pos += total_req_len;
        c->qb_applied = c->qb_pos;
        c->net_input_bytes_curr_cmd = total_req_len;
        c->argc = 0;
        if (args) {
            for (int i = 0; i < argc; i++) sdsfree(args[i]);
            zfree(args);
        }
        return;
    }

    /* Assign arguments to client structure */
    if (c->argv) zfree(c->argv);
    c->argv = zmalloc(sizeof(robj *) * argc);
    c->argc = argc;
    c->argv_len = argc;
    c->argv_len_sum = 0;
    for (int i = 0; i < argc; i++) {
        args[i] = sdsRemoveFreeSpace(args[i], 1);
        c->argv[i] = createObject(OBJ_STRING, args[i]);
        c->argv_len_sum += sdslen(args[i]);
    }
    zfree(args);

    c->qb_pos += total_req_len;
    c->qb_applied = c->qb_pos;
    c->net_input_bytes_curr_cmd = total_req_len;
    c->read_flags |= READ_FLAGS_PARSING_COMPLETED;
    c->reqtype = 0;
}

/* Intercept RESP output from client and format as HTTP JSON response */
void processRestReply(client *c) {
    sds resp = aggregateClientOutputBuffer(c);

    /* Discard raw RESP reply from buffers */
    releaseReplyReferences(c);
    resetLastWrittenBuf(c);
    c->bufpos = 0;
    c->flag.buf_encoded = 0;
    c->last_header = NULL;
    c->reply_bytes = 0;
    listEmpty(c->reply);

    int is_error = 0;
    const char *ptr = resp;
    const char *end = resp + sdslen(resp);
    sds json = parseRespValue(&ptr, end, &is_error);

    int status_code = is_error ? 400 : 200;
    const char *status_phrase = is_error ? "Bad Request" : "OK";

    sds body_json = sdsempty();
    if (is_error) {
        body_json = sdscatprintf(body_json, "{\"ok\":false,\"error\":%s}\n", json);
    } else {
        body_json = sdscatprintf(body_json, "{\"ok\":true,\"result\":%s}\n", json);
    }

    int close_conn = c->flag.close_after_reply || !c->flag.rest_keep_alive;
    sds http = sdscatprintf(sdsempty(),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, HEAD\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Connection: %s\r\n"
        "\r\n"
        "%s",
        status_code, status_phrase,
        sdslen(body_json),
        close_conn ? "close" : "keep-alive",
        body_json);

    addReplyProto(c, http, sdslen(http));

    sdsfree(http);
    sdsfree(body_json);
    sdsfree(json);
    sdsfree(resp);

    if (close_conn) {
        c->flag.close_after_reply = 1;
    }
}

/* Event handler for accepting connections on dedicated REST listener */
static void acceptRestHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = server.max_new_conns_per_cycle;
    struct ClientFlags flags = {0};
    flags.rest_client = 1;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (anetRetryAcceptOnError(errno)) continue;
            if (errno != EWOULDBLOCK) serverLog(LL_WARNING, "Accepting REST client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE, "Accepted REST %s:%d", cip, cport);

        if (server.tcpkeepalive) anetKeepAlive(NULL, cfd, server.tcpkeepalive);
        acceptCommonHandler(connCreateAccepted(connectionByType(CONN_TYPE_SOCKET), cfd, NULL), flags, cip);
    }
}

/* Initialize dedicated REST listener if rest_port is configured */
void initRestListener(void) {
    if (server.rest_port == 0) return;
    ConnectionType *ct = connectionByType(CONN_TYPE_SOCKET);
    if (!ct) return;

    memset(&server.rest_listener, 0, sizeof(server.rest_listener));
    server.rest_listener.bindaddr = server.bindaddr;
    server.rest_listener.bindaddr_count = server.bindaddr_count;
    server.rest_listener.port = server.rest_port;
    server.rest_listener.ct = ct;

    if (connListen(&server.rest_listener) != C_OK) {
        serverLog(LL_WARNING, "Failed listening on REST port %u, aborting.", server.rest_port);
        exit(1);
    }
    if (createSocketAcceptHandler(&server.rest_listener, acceptRestHandler) != C_OK) {
        serverPanic("Unrecoverable error creating REST listener accept handler.");
    }
    serverLog(LL_NOTICE, "REST web server listening on port %d", server.rest_port);
}

/* Clean up dedicated REST listener */
void cleanRestListener(void) {
    if (server.rest_port == 0) return;
    for (int j = 0; j < server.rest_listener.count; j++) {
        if (server.rest_listener.fd[j] != -1) {
            aeDeleteFileEvent(server.el, server.rest_listener.fd[j], AE_READABLE);
            close(server.rest_listener.fd[j]);
            server.rest_listener.fd[j] = -1;
        }
    }
    server.rest_listener.count = 0;
}
