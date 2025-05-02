#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
// proj
#include "../include/common.h"


static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

const size_t k_max_msg = 4096;

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        len += 4 + s.size();
    }
    if (len > k_max_msg) {
        return -1;
    }

    char wbuf[4 + k_max_msg];
    memcpy(&wbuf[0], &len, 4);  // assume little endian
    uint32_t n = cmd.size();
    memcpy(&wbuf[4], &n, 4);
    size_t cur = 8;
    for (const std::string &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf, 4 + len);
}

static int32_t on_response(const uint8_t *data, size_t size) {
    if (size < 1) {
        msg("bad response");
        return -1;
    }
    switch (data[0]) {
    case '+':  // Simple string
        {
            const char *end = (const char *)memchr(data + 1, '\r', size - 1);
            if (!end || end[1] != '\n') {
                msg("bad response");
                return -1;
            }
            printf("%.*s\n", (int)(end - (const char *)data - 1), data + 1);
            return end - (const char *)data + 2;
        }
    case '-':  // Error
        {
            const char *end = (const char *)memchr(data + 1, '\r', size - 1);
            if (!end || end[1] != '\n') {
                msg("bad response");
                return -1;
            }
            printf("(error) %.*s\n", (int)(end - (const char *)data - 1), data + 1);
            return end - (const char *)data + 2;
        }
    case ':':  // Integer
        {
            const char *end = (const char *)memchr(data + 1, '\r', size - 1);
            if (!end || end[1] != '\n') {
                msg("bad response");
                return -1;
            }
            printf("(integer) %.*s\n", (int)(end - (const char *)data - 1), data + 1);
            return end - (const char *)data + 2;
        }
    case '$':  // Bulk string
        {
            const char *p = (const char *)data + 1;
            const char *end = (const char *)memchr(p, '\r', size - (p - (const char *)data));
            if (!end || end[1] != '\n') {
                msg("bad response");
                return -1;
            }
            int len = atoi(std::string(p, end - p).c_str());
            if (len < 0) {
                printf("(nil)\n");
                return end - (const char *)data + 2;
            }
            p = end + 2;
            if (p + len + 2 > (const char *)data + size) {
                msg("bad response");
                return -1;
            }
            if (p[len] != '\r' || p[len + 1] != '\n') {
                msg("bad response");
                return -1;
            }
            printf("%.*s\n", len, p);
            return p + len + 2 - (const char *)data;
        }
    case '*':  // Array
        {
            const char *p = (const char *)data + 1;
            const char *end = (const char *)memchr(p, '\r', size - (p - (const char *)data));
            if (!end || end[1] != '\n') {
                msg("bad response");
                return -1;
            }
            int len = atoi(std::string(p, end - p).c_str());
            if (len < 0) {
                printf("(nil)\n");
                return end - (const char *)data + 2;
            }
            p = end + 2;
            printf("(array) len=%d\n", len);
            for (int i = 0; i < len; ++i) {
                int32_t rv = on_response((const uint8_t *)p, size - (p - (const char *)data));
                if (rv < 0) {
                    return rv;
                }
                p += rv;
            }
            return p - (const char *)data;
        }
    default:
        msg("bad response");
        return -1;
    }
}

static int32_t read_res(int fd) {
    // 4 bytes header
    char rbuf[4 + k_max_msg + 1];
    errno = 0;
    int32_t err = read_full(fd, rbuf, 4);
    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    // reply body
    err = read_full(fd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // print the result
    int32_t rv = on_response((uint8_t *)&rbuf[4], len);
    if (rv > 0 && (uint32_t)rv != len) {
        msg("bad response");
        rv = -1;
    }
    return rv;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <server_ip> <server_port> [command...]\n", argv[0]);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    addr.sin_addr.s_addr = inet_addr(argv[1]);

    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

    // Send command
    std::vector<std::string> cmd;
    for (int i = 3; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }

    if (cmd.empty()) {
        // Interactive mode
        char line[1024];
        while (fgets(line, sizeof(line), stdin)) {
            // Parse command
            cmd.clear();
            char *p = line;
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == '\n') {
                    *p++ = '\0';
                }
                if (*p) {
                    cmd.push_back(p);
                    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
                        ++p;
                    }
                }
            }

            if (cmd.empty()) {
                continue;
            }

            // Send request
            int32_t err = send_req(fd, cmd);
            if (err) {
                goto L_DONE;
            }

            // Read response
            err = read_res(fd);
            if (err) {
                goto L_DONE;
            }
        }
    } else {
        // Command-line mode
        int32_t err = send_req(fd, cmd);
        if (err) {
            goto L_DONE;
        }

        err = read_res(fd);
        if (err) {
            goto L_DONE;
        }
    }

L_DONE:
    close(fd);
    return 0;
}
