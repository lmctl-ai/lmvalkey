proc http_request {host port req} {
    set fd [socket $host $port]
    fconfigure $fd -translation binary -blocking 1
    puts -nonewline $fd $req
    flush $fd
    set response [read $fd]
    close $fd
    return $response
}

start_server {tags {"rest network"}} {
    set host [srv 0 host]
    set port [srv 0 port]

    test "REST: GET / health check" {
        set res [http_request $host $port "GET / HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res
        assert_match "*\"server\":\"valkey\"*" $res
    }

    test "REST: OPTIONS preflight" {
        set res [http_request $host $port "OPTIONS /set/a/b HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 204 No Content*" $res
        assert_match "*Access-Control-Allow-Origin: \**" $res
    }

    test "REST: GET /ping" {
        set res [http_request $host $port "GET /ping HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res
        assert_match "*\"ok\":true,\"result\":\"PONG\"*" $res
    }

    test "REST: POST /set/testkey/testval and GET /get/testkey" {
        set res1 [http_request $host $port "POST /set/testkey/testval HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res1
        assert_match "*\"ok\":true,\"result\":\"OK\"*" $res1

        set res2 [http_request $host $port "GET /get/testkey HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res2
        assert_match "*\"ok\":true,\"result\":\"testval\"*" $res2
    }

    test "REST: RESTful /keys/<key> endpoint" {
        set body "{\"user\":\"charlie\"}"
        set len [string length $body]
        set req "PUT /keys/user:200 HTTP/1.1\r\nHost: $host\r\nContent-Length: $len\r\nConnection: close\r\n\r\n$body"
        set res1 [http_request $host $port $req]
        assert_match "*HTTP/1.1 200 OK*" $res1

        set res2 [http_request $host $port "GET /keys/user:200 HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res2
        assert_match "*charlie*" $res2

        set res3 [http_request $host $port "DELETE /keys/user:200 HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res3
        assert_match "*\"result\":1*" $res3

        set res4 [http_request $host $port "GET /keys/user:200 HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*\"result\":null*" $res4
    }

    test "REST: POST /cmd with JSON array" {
        set body "\[\"MSET\", \"k1\", \"v1\", \"k2\", \"v2\"\]"
        set len [string length $body]
        set req "POST /cmd HTTP/1.1\r\nHost: $host\r\nContent-Type: application/json\r\nContent-Length: $len\r\nConnection: close\r\n\r\n$body"
        set res1 [http_request $host $port $req]
        assert_match "*HTTP/1.1 200 OK*" $res1
        assert_match "*\"ok\":true,\"result\":\"OK\"*" $res1

        set body2 "\[\"MGET\", \"k1\", \"k2\"\]"
        set len2 [string length $body2]
        set req2 "POST /cmd HTTP/1.1\r\nHost: $host\r\nContent-Type: application/json\r\nContent-Length: $len2\r\nConnection: close\r\n\r\n$body2"
        set res2 [http_request $host $port $req2]
        assert_match "*HTTP/1.1 200 OK*" $res2
        assert_match "*v1*v2*" $res2
    }

    test "REST: Error response on unknown command" {
        set res [http_request $host $port "GET /unknowncommand123 HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 400 Bad Request*" $res
        assert_match "*\"ok\":false*" $res
    }
}

start_server {tags {"rest network"} overrides {requirepass secretpass}} {
    set host [srv 0 host]
    set port [srv 0 port]

    test "REST: Auth bypass when server has requirepass" {
        set rr [valkey [srv "host"] [srv "port"] 0 $::tls]
        catch {[$rr ping]} err
        assert_match "*NOAUTH*" $err
        $rr close

        # REST request should succeed without auth
        set res [http_request $host $port "GET /ping HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res
        assert_match "*\"ok\":true,\"result\":\"PONG\"*" $res

        # REST command write and read should execute without auth
        set res2 [http_request $host $port "POST /set/sec/hello HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res2

        set res3 [http_request $host $port "GET /get/sec HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n"]
        assert_match "*HTTP/1.1 200 OK*" $res3
        assert_match "*\"result\":\"hello\"*" $res3
    }
}
