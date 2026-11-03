// lmcode host integration for lmvalkey.
// Embeds dslvm.hpp and exposes Valkey KV and Batch operations to lmscript _lmcode_ blocks.

#include "include/dslvm.hpp"
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// The embedded schema bytes for lmvalkey
static const char LMVALKEY_SCHEMA_JSON[] = R"JSON({
  "dsl.schema": 2,
  "name": "lmvalkey",
  "types": {
    "Valkey": {
      "methods": {
        "set": [
          { "params": ["string", "string"], "returns": "bool" }
        ],
        "get": [
          { "params": ["string"], "returns": "string" }
        ],
        "del": [
          { "params": ["string"], "returns": "int" }
        ],
        "exists": [
          { "params": ["string"], "returns": "bool" }
        ],
        "cmd": [
          { "params": ["string"], "returns": "string" },
          { "params": ["string", "string"], "returns": "string" },
          { "params": ["string", "string", "string"], "returns": "string" }
        ],
        "batch": [
          { "params": [], "returns": "Batch" }
        ]
      }
    },
    "Batch": {
      "methods": {
        "set": [
          { "params": ["string", "string"], "returns": "Batch" }
        ],
        "get": [
          { "params": ["string"], "returns": "Batch" }
        ],
        "del": [
          { "params": ["string"], "returns": "Batch" }
        ],
        "cmd": [
          { "params": ["string"], "returns": "Batch" },
          { "params": ["string", "string"], "returns": "Batch" },
          { "params": ["string", "string", "string"], "returns": "Batch" }
        ],
        "len": [
          { "params": [], "returns": "int" }
        ],
        "clear": [
          { "params": [], "returns": "void" }
        ],
        "exec": [
          { "params": [], "returns": "string" }
        ]
      }
    }
  },
  "functions": {
    "print": [
      { "params": ["string"], "returns": "void" }
    ]
  },
  "context": {
    "valkey": "Valkey"
  }
}
)JSON";

class Batch;

class ValkeyClient {
public:
    std::string host = "127.0.0.1";
    int port = 6379;
    int sock_fd = -1;
    bool checked_connection = false;
    bool is_live_server = false;
    std::map<std::string, std::string> fallback_store;
    std::vector<std::unique_ptr<Batch>> batches;

    ValkeyClient() {
        const char *p = std::getenv("VALKEY_PORT");
        if (!p) p = std::getenv("REDIS_PORT");
        if (p) port = std::atoi(p);
        const char *h = std::getenv("VALKEY_HOST");
        if (!h) h = std::getenv("REDIS_HOST");
        if (h) host = h;
    }

    ~ValkeyClient() {
        close_socket();
    }

    void close_socket() {
        if (sock_fd >= 0) {
#ifdef _WIN32
            closesocket(sock_fd);
#else
            close(sock_fd);
#endif
            sock_fd = -1;
        }
    }

    bool ensure_connected() {
        if (checked_connection) return is_live_server;
        checked_connection = true;

#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
#endif

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            is_live_server = false;
            return false;
        }

        struct sockaddr_in sin;
        std::memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &sin.sin_addr);

        if (connect(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            is_live_server = false;
            return false;
        }

        sock_fd = fd;
        is_live_server = true;
        return true;
    }

    static std::string format_resp(const std::vector<std::string>& args) {
        std::string out = "*" + std::to_string(args.size()) + "\r\n";
        for (const auto& a : args) {
            out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        }
        return out;
    }

    std::string read_resp_reply() {
        if (sock_fd < 0) throw dslvm::Error("not connected to server");
        char type_c;
        if (recv(sock_fd, &type_c, 1, 0) <= 0) {
            throw dslvm::Error("connection closed by server");
        }

        auto read_line = [&]() -> std::string {
            std::string line;
            char ch;
            while (recv(sock_fd, &ch, 1, 0) > 0) {
                if (ch == '\r') {
                    recv(sock_fd, &ch, 1, 0); // read '\n'
                    break;
                }
                line.push_back(ch);
            }
            return line;
        };

        if (type_c == '+' || type_c == ':') {
            return read_line();
        } else if (type_c == '-') {
            std::string err = read_line();
            throw dslvm::Error(err);
        } else if (type_c == '$') {
            std::string len_str = read_line();
            int64_t len = std::atoll(len_str.c_str());
            if (len < 0) return "";
            std::string val(len, '\0');
            size_t total = 0;
            while (total < (size_t)len) {
                ssize_t n = recv(sock_fd, &val[total], len - total, 0);
                if (n <= 0) throw dslvm::Error("unexpected EOF in bulk string");
                total += n;
            }
            char crlf[2];
            recv(sock_fd, crlf, 2, 0);
            return val;
        } else if (type_c == '*') {
            std::string count_str = read_line();
            int64_t count = std::atoll(count_str.c_str());
            if (count < 0) return "null";
            std::string res = "[";
            for (int64_t i = 0; i < count; ++i) {
                if (i > 0) res += ",";
                res += read_resp_reply();
            }
            res += "]";
            return res;
        }
        return read_line();
    }

    std::string exec_command(const std::vector<std::string>& args) {
        if (ensure_connected()) {
            std::string req = format_resp(args);
            if (send(sock_fd, req.data(), req.size(), 0) < 0) {
                close_socket();
                throw dslvm::Error("send failed to valkey server");
            }
            return read_resp_reply();
        }

        // Standalone in-memory fallback
        if (args.empty()) return "";
        std::string cmd = args[0];
        for (auto& c : cmd) c = toupper((unsigned char)c);

        if (cmd == "PING") {
            return args.size() > 1 ? args[1] : "PONG";
        } else if (cmd == "SET" && args.size() >= 3) {
            fallback_store[args[1]] = args[2];
            return "OK";
        } else if (cmd == "GET" && args.size() >= 2) {
            auto it = fallback_store.find(args[1]);
            return it != fallback_store.end() ? it->second : "";
        } else if (cmd == "DEL" && args.size() >= 2) {
            size_t n = fallback_store.erase(args[1]);
            return std::to_string(n);
        } else if (cmd == "EXISTS" && args.size() >= 2) {
            return fallback_store.count(args[1]) ? "1" : "0";
        }
        return "OK";
    }

    std::string exec_batch(const std::vector<std::vector<std::string>>& batch) {
        if (ensure_connected()) {
            std::string all_req;
            for (const auto& cmd : batch) {
                all_req += format_resp(cmd);
            }
            if (!all_req.empty() && send(sock_fd, all_req.data(), all_req.size(), 0) < 0) {
                close_socket();
                throw dslvm::Error("batch send failed to valkey server");
            }
            std::string res = "[";
            for (size_t i = 0; i < batch.size(); ++i) {
                if (i > 0) res += ",";
                std::string reply = read_resp_reply();
                // Escape JSON string
                res += "\"";
                for (char c : reply) {
                    if (c == '"') res += "\\\"";
                    else if (c == '\\') res += "\\\\";
                    else if (c == '\n') res += "\\n";
                    else if (c == '\r') res += "\\r";
                    else res.push_back(c);
                }
                res += "\"";
            }
            res += "]";
            return res;
        }

        // In-memory fallback
        std::string res = "[";
        for (size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) res += ",";
            std::string reply = exec_command(batch[i]);
            res += "\"";
            for (char c : reply) {
                if (c == '"') res += "\\\"";
                else if (c == '\\') res += "\\\\";
                else if (c == '\n') res += "\\n";
                else if (c == '\r') res += "\\r";
                else res.push_back(c);
            }
            res += "\"";
        }
        res += "]";
        return res;
    }

    Batch* create_batch();
};

class Batch {
public:
    ValkeyClient *client;
    std::vector<std::vector<std::string>> commands;

    Batch(ValkeyClient *c) : client(c) {}

    Batch* set(const std::string& k, const std::string& v) {
        commands.push_back({"SET", k, v});
        return this;
    }

    Batch* get(const std::string& k) {
        commands.push_back({"GET", k});
        return this;
    }

    Batch* del(const std::string& k) {
        commands.push_back({"DEL", k});
        return this;
    }

    Batch* cmd1(const std::string& c) {
        commands.push_back({c});
        return this;
    }

    Batch* cmd2(const std::string& c, const std::string& a1) {
        commands.push_back({c, a1});
        return this;
    }

    Batch* cmd3(const std::string& c, const std::string& a1, const std::string& a2) {
        commands.push_back({c, a1, a2});
        return this;
    }

    int64_t len() const {
        return commands.size();
    }

    void clear() {
        commands.clear();
    }

    std::string exec() {
        return client->exec_batch(commands);
    }
};

Batch* ValkeyClient::create_batch() {
    batches.push_back(std::make_unique<Batch>(this));
    return batches.back().get();
}

static void register_lmvalkey_schema(dslvm::Registry& reg) {
    reg.context("valkey", "Valkey");

    // Valkey methods
    reg.method("Valkey", "set", {"string", "string"}, "bool", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        std::string r = v->exec_command({"SET", a[0].s, a[1].s});
        return dslvm::Value::boolean(r == "OK");
    });

    reg.method("Valkey", "get", {"string"}, "string", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        return dslvm::Value::string(v->exec_command({"GET", a[0].s}));
    });

    reg.method("Valkey", "del", {"string"}, "int", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        std::string r = v->exec_command({"DEL", a[0].s});
        return dslvm::Value::integer(std::atoll(r.c_str()));
    });

    reg.method("Valkey", "exists", {"string"}, "bool", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        std::string r = v->exec_command({"EXISTS", a[0].s});
        return dslvm::Value::boolean(r == "1");
    });

    reg.method("Valkey", "cmd", {"string"}, "string", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        return dslvm::Value::string(v->exec_command({a[0].s}));
    });

    reg.method("Valkey", "cmd", {"string", "string"}, "string", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        return dslvm::Value::string(v->exec_command({a[0].s, a[1].s}));
    });

    reg.method("Valkey", "cmd", {"string", "string", "string"}, "string", [](void* self, dslvm::Args& a) {
        auto* v = static_cast<ValkeyClient*>(self);
        return dslvm::Value::string(v->exec_command({a[0].s, a[1].s, a[2].s}));
    });

    reg.method("Valkey", "batch", {}, "Batch", [](void* self, dslvm::Args&) {
        auto* v = static_cast<ValkeyClient*>(self);
        return dslvm::Value::object("Batch", v->create_batch());
    });

    // Batch methods
    reg.method("Batch", "set", {"string", "string"}, "Batch", [](void* self, dslvm::Args& a) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::object("Batch", b->set(a[0].s, a[1].s));
    });

    reg.method("Batch", "get", {"string"}, "Batch", [](void* self, dslvm::Args& a) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::object("Batch", b->get(a[0].s));
    });

    reg.method("Batch", "del", {"string"}, "Batch", [](void* self, dslvm::Args& a) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::object("Batch", b->del(a[0].s));
    });

    reg.method("Batch", "cmd", {"string"}, "Batch", [](void* self, dslvm::Args& a) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::object("Batch", b->cmd1(a[0].s));
    });

    reg.method("Batch", "cmd", {"string", "string"}, "Batch", [](void* self, dslvm::Args& a) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::object("Batch", b->cmd2(a[0].s, a[1].s));
    });

    reg.method("Batch", "cmd", {"string", "string", "string"}, "Batch", [](void* self, dslvm::Args& a) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::object("Batch", b->cmd3(a[0].s, a[1].s, a[2].s));
    });

    reg.method("Batch", "len", {}, "int", [](void* self, dslvm::Args&) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::integer(b->len());
    });

    reg.method("Batch", "clear", {}, "void", [](void* self, dslvm::Args&) {
        auto* b = static_cast<Batch*>(self);
        b->clear();
        return dslvm::Value::none();
    });

    reg.method("Batch", "exec", {}, "string", [](void* self, dslvm::Args&) {
        auto* b = static_cast<Batch*>(self);
        return dslvm::Value::string(b->exec());
    });

    // Host functions
    reg.function("print", {"string"}, "void", [](dslvm::Args& a) {
        std::cout << a[0].s << "\n";
        return dslvm::Value::none();
    });
}

extern "C" {

void lmcode_print_schema(int json) {
    (void)json;
    std::cout << LMVALKEY_SCHEMA_JSON;
}

void lmcode_print_signatures(void) {
    dslvm::Registry reg;
    register_lmvalkey_schema(reg);
    for (const auto& sig : reg.signatures()) {
        std::cout << sig << "\n";
    }
}

int lmcode_run(int argc, char **argv) {
    std::string bytecode_file;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--lmcode") == 0) {
            if (i + 1 < argc) {
                bytecode_file = argv[++i];
            }
        }
    }

    if (bytecode_file.empty()) {
        std::cout << "LMCODE_RETURN_BEGIN\n"
                  << "{\"ok\":false,\"phase\":\"cli\",\"code\":\"CLI_USAGE\",\"message\":\"missing --lmcode <file>\"}\n"
                  << "LMCODE_RETURN_END\n";
        return 2;
    }

    dslvm::Registry reg;
    register_lmvalkey_schema(reg);

    dslvm::Program program;
    try {
        std::string bytecode = dslvm::read_file(bytecode_file);
        program = dslvm::Program::load(bytecode, LMVALKEY_SCHEMA_JSON, reg);
    } catch (const dslvm::Error& e) {
        std::cout << "LMCODE_RETURN_BEGIN\n"
                  << "{\"ok\":false,\"phase\":\"load\",\"code\":\"" << e.code << "\",\"message\":\"" << e.what() << "\"}\n"
                  << "LMCODE_RETURN_END\n";
        return 3;
    }

    ValkeyClient client;
    dslvm::Context ctx;
    ctx.set("valkey", &client);

    try {
        program.run(ctx, reg);
    } catch (const dslvm::Error& e) {
        std::cout << "LMCODE_RETURN_BEGIN\n"
                  << "{\"ok\":false,\"phase\":\"run\",\"code\":\"" << e.code << "\",\"message\":\"" << e.what() << "\"}\n"
                  << "LMCODE_RETURN_END\n";
        return 3;
    }

    std::cout << "LMCODE_RETURN_BEGIN\n"
              << "{\"ok\":true,\"result\":\"ok\"}\n"
              << "LMCODE_RETURN_END\n";
    return 0;
}

} // extern "C"
