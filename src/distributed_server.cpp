#include <mpi.h>
#include <omp.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "../include/hashtable.h"
#include "../include/zset.h"
#include "../include/avl.h"
#include "../include/heap.h"
#include "../include/thread_pool.h"
#include "../include/list.h"
#include "../include/common.h"

// Constants
const int MAX_NODES = 32;  // Maximum number of nodes in the cluster
const int VIRTUAL_NODES = 100;  // Number of virtual nodes per physical node
const int MAX_MSG_SIZE = 4096;  // Maximum message size for MPI communication
const int k_max_msg = 4096;
const int k_idle_timeout_ms = 5 * 1000;

// Message types for MPI communication
enum MessageType {
    MSG_GET = 1,
    MSG_SET = 2,
    MSG_DEL = 3,
    MSG_RESPONSE = 4,
    MSG_SYNC = 5
};

// Connection states
enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_WAIT = 2,  // waiting for remote response
    STATE_END = 3,   // mark the connection for deletion
};

// Error codes
enum {
    ERR_UNKNOWN = 1,
    ERR_2BIG = 2,
    ERR_TYPE = 3,
    ERR_ARG = 4,
};

// Entry types
enum {
    T_STR = 0,
    T_ZSET = 1,
};

// Structure for consistent hashing
struct VirtualNode {
    int physical_node;
    uint32_t hash;
};

// Connection structure
struct Conn {
    int fd = -1;
    uint32_t state = 0;     // either STATE_REQ or STATE_RES
    // buffer for reading
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    // buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
    uint64_t idle_start = 0;
    // timer
    DList idle_list;
};

// Entry structure
struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
    uint32_t type = 0;
    ZSet *zset = NULL;
    // for TTLs
    size_t heap_idx = -1;
};

// Global state
struct {
    HMap db;
    std::vector<Conn *> fd2conn;
    DList idle_list;
    std::vector<HeapItem> heap;
    TheadPool tp;
    
    // Distributed system state
    int rank;  // Current node's rank
    int size;  // Total number of nodes
    std::vector<VirtualNode> virtual_nodes;  // Consistent hashing ring
    std::unordered_map<std::string, int> key_to_node;  // Cache for key-to-node mapping
} g_data;

// Forward declarations
static void out_nil(std::string &out);
static void out_str(std::string &out, const std::string &val);
static void out_int(std::string &out, int64_t val);
static void out_err(std::string &out, int32_t code, const std::string &msg);
static void out_arr(std::string &out, uint32_t n);
static void entry_del(Entry *ent);
static void entry_set_ttl(Entry *ent, int64_t ttl_ms);
static bool try_fill_buffer(Conn *conn);
static bool try_one_request(Conn *conn);
static bool try_flush_buffer(Conn *conn);
static void state_res(Conn *conn);
static int parse_req(const uint8_t *data, size_t len, std::vector<std::string> &out);
static void hm_init(HMap *hmap);

// Helper functions
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static uint64_t get_monotonic_usec() {
    timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return uint64_t(tv.tv_sec) * 1000000 + tv.tv_nsec / 1000;
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

static bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

// Hash function for consistent hashing
uint32_t hash_key(const std::string& key) {
    return std::hash<std::string>{}(key);
}

// Initialize the consistent hashing ring
void init_consistent_hashing() {
    g_data.virtual_nodes.clear();
    for (int i = 0; i < g_data.size; ++i) {
        for (int j = 0; j < VIRTUAL_NODES; ++j) {
            std::string node_key = "node_" + std::to_string(i) + "_" + std::to_string(j);
            VirtualNode vnode;
            vnode.physical_node = i;
            vnode.hash = hash_key(node_key);
            g_data.virtual_nodes.push_back(vnode);
        }
    }
    std::sort(g_data.virtual_nodes.begin(), g_data.virtual_nodes.end(),
              [](const VirtualNode& a, const VirtualNode& b) { return a.hash < b.hash; });
}

// Find the node responsible for a key
int find_node_for_key(const std::string& key) {
    uint32_t key_hash = hash_key(key);
    auto it = std::lower_bound(g_data.virtual_nodes.begin(), g_data.virtual_nodes.end(),
                              key_hash,
                              [](const VirtualNode& node, uint32_t hash) {
                                  return node.hash < hash;
                              });
    if (it == g_data.virtual_nodes.end()) {
        it = g_data.virtual_nodes.begin();
    }
    return it->physical_node;
}

// Send a message to another node
void send_message(int dest, MessageType type, const std::string& key, const std::string& value = "") {
    char buffer[MAX_MSG_SIZE];
    int pos = 0;
    
    // Pack message type
    memcpy(buffer + pos, &type, sizeof(type));
    pos += sizeof(type);
    
    // Pack key length and key
    uint32_t key_len = key.length();
    memcpy(buffer + pos, &key_len, sizeof(key_len));
    pos += sizeof(key_len);
    memcpy(buffer + pos, key.c_str(), key_len);
    pos += key_len;
    
    // Pack value length and value (if present)
    if (!value.empty()) {
        uint32_t value_len = value.length();
        memcpy(buffer + pos, &value_len, sizeof(value_len));
        pos += sizeof(value_len);
        memcpy(buffer + pos, value.c_str(), value_len);
        pos += value_len;
    }
    
    MPI_Send(buffer, pos, MPI_CHAR, dest, 0, MPI_COMM_WORLD);
}

// Receive a message from any node
void receive_message(int* source, MessageType* type, std::string& key, std::string& value) {
    char buffer[MAX_MSG_SIZE];
    MPI_Status status;
    MPI_Recv(buffer, MAX_MSG_SIZE, MPI_CHAR, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
    *source = status.MPI_SOURCE;
    
    int pos = 0;
    
    // Unpack message type
    memcpy(type, buffer + pos, sizeof(*type));
    pos += sizeof(*type);
    
    // Unpack key
    uint32_t key_len;
    memcpy(&key_len, buffer + pos, sizeof(key_len));
    pos += sizeof(key_len);
    key.assign(buffer + pos, key_len);
    pos += key_len;
    
    // Unpack value if present
    if (*type == MSG_SET || *type == MSG_RESPONSE) {
        uint32_t value_len;
        memcpy(&value_len, buffer + pos, sizeof(value_len));
        pos += sizeof(value_len);
        value.assign(buffer + pos, value_len);
    }
}

// Process incoming messages from other nodes
void process_incoming_messages() {
    int flag;
    MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    if (!flag) {
        return;
    }

    int source;
    MessageType type;
    std::string key, value;
    receive_message(&source, &type, key, value);

    switch (type) {
    case MSG_GET: {
        HNode node;
        node.hcode = hash_key(key);
        Entry* ent = (Entry*)hm_lookup(&g_data.db, &node, entry_eq);
        if (ent) {
            send_message(source, MSG_RESPONSE, key, ent->val);
        } else {
            send_message(source, MSG_RESPONSE, key, "");
        }
        break;
    }
    case MSG_SET: {
        HNode node;
        node.hcode = hash_key(key);
        Entry* ent = (Entry*)hm_lookup(&g_data.db, &node, entry_eq);
        if (ent) {
            ent->val = value;
        } else {
            ent = new Entry();
            ent->key = key;
            ent->val = value;
            ent->node.hcode = hash_key(key);
            hm_insert(&g_data.db, &ent->node);
        }
        send_message(source, MSG_RESPONSE, key, "OK");
        break;
    }
    case MSG_DEL: {
        HNode node;
        node.hcode = hash_key(key);
        Entry* ent = (Entry*)hm_lookup(&g_data.db, &node, entry_eq);
        if (ent) {
            hm_pop(&g_data.db, &ent->node, entry_eq);
            delete ent;
            send_message(source, MSG_RESPONSE, key, "OK");
        } else {
            send_message(source, MSG_RESPONSE, key, "OK");
        }
        break;
    }
    case MSG_RESPONSE:
        // Responses are handled in the command functions
        break;
    case MSG_SYNC:
        // Handle sync messages if needed
        break;
    }
}

// Mark unused functions as used
[[maybe_unused]] static void do_get(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_ARG, "wrong number of arguments");
        return;
    }
    
    std::string key = cmd[1];
    int target_node = find_node_for_key(key);
    
    if (target_node == g_data.rank) {
        // Local
        HNode node;
        node.hcode = hash_key(key);
        Entry* ent = (Entry*)hm_lookup(&g_data.db, &node, entry_eq);
        if (ent) {
            out_str(out, ent->val);
        } else {
            out_nil(out);
        }
    } else {
        // Remote
        send_message(target_node, MSG_GET, key);
        
        // Wait for response
        int source;
        MessageType type;
        std::string response;
        receive_message(&source, &type, key, response);
        
        if (response.empty()) {
            out_nil(out);
        } else {
            out_str(out, response);
        }
    }
}

// Mark unused functions as used
[[maybe_unused]] static void do_set(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_ARG, "wrong number of arguments");
        return;
    }
    
    std::string key = cmd[1];
    std::string val = cmd[2];
    int target_node = find_node_for_key(key);
    
    if (target_node == g_data.rank) {
        // Local
        HNode node;
        node.hcode = hash_key(key);
        Entry* ent = (Entry*)hm_lookup(&g_data.db, &node, entry_eq);
        if (ent) {
            ent->val = val;
        } else {
            ent = new Entry();
            ent->key = key;
            ent->val = val;
            ent->node.hcode = hash_key(key);
            hm_insert(&g_data.db, &ent->node);
        }
        out_str(out, "OK");
    } else {
        // Remote
        send_message(target_node, MSG_SET, key, val);
        
        // Set connection state to WAIT
        Conn* conn = g_data.fd2conn[0];  // Get the current connection
        if (conn) {
            conn->state = STATE_WAIT;
        }
        
        // Wait for response with timeout
        int max_attempts = 10;
        int attempts = 0;
        bool got_response = false;
        
        while (attempts < max_attempts) {
            // Process any incoming messages
            process_incoming_messages();
            
            // Check for response
            int flag;
            MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
            if (flag) {
                int source;
                MessageType type;
                std::string response_key, response_val;
                receive_message(&source, &type, response_key, response_val);
                
                if (type == MSG_RESPONSE && response_key == key) {
                    if (response_val == "OK") {
                        out_str(out, "OK");
                    } else {
                        out_err(out, ERR_UNKNOWN, "Failed to set key on remote node");
                    }
                    got_response = true;
                    break;
                }
            }
            
            // Sleep for a short time to avoid busy waiting
            usleep(100000); // 100ms
            attempts++;
            
            // Check if the connection is still valid
            if (g_data.fd2conn.empty() || g_data.fd2conn[0] == nullptr) {
                msg("Connection closed while waiting for response");
                return;
            }
        }
        
        if (!got_response) {
            out_err(out, ERR_UNKNOWN, "Timeout waiting for response from remote node");
        }
        
        // Reset connection state back to REQ
        if (conn) {
            conn->state = STATE_REQ;
        }
    }
}

// Mark unused functions as used
[[maybe_unused]] static void do_del(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_ARG, "wrong number of arguments");
        return;
    }
    
    std::string key = cmd[1];
    int target_node = find_node_for_key(key);
    
    if (target_node == g_data.rank) {
        // Local
        HNode node;
        node.hcode = hash_key(key);
        Entry* ent = (Entry*)hm_lookup(&g_data.db, &node, entry_eq);
        if (ent) {
            hm_pop(&g_data.db, &ent->node, entry_eq);
            delete ent;
            out_int(out, 1);
        } else {
            out_int(out, 0);
        }
    } else {
        // Remote
        send_message(target_node, MSG_DEL, key);
        out_int(out, 1);  // Assume success for remote operations
    }
}

// Mark unused functions as used
[[maybe_unused]] static void out_nil(std::string &out) {
    out.push_back('_');
    out.push_back('\r');
    out.push_back('\n');
}

// Mark unused functions as used
[[maybe_unused]] static void out_str(std::string &out, const std::string &val) {
    out.push_back('$');
    out.append(std::to_string(val.size()));
    out.push_back('\r');
    out.push_back('\n');
    out.append(val);
    out.push_back('\r');
    out.push_back('\n');
}

// Mark unused functions as used
[[maybe_unused]] static void out_int(std::string &out, int64_t val) {
    out.push_back(':');
    out.append(std::to_string(val));
    out.push_back('\r');
    out.push_back('\n');
}

// Mark unused functions as used
[[maybe_unused]] static void out_err(std::string &out, int32_t code, const std::string &msg) {
    out.push_back('-');
    out.append(std::to_string(code));
    out.push_back(' ');
    out.append(msg);
    out.push_back('\r');
    out.push_back('\n');
}

// Mark unused functions as used
[[maybe_unused]] static void out_arr(std::string &out, uint32_t n) {
    out.push_back('*');
    out.append(std::to_string(n));
    out.push_back('\r');
    out.push_back('\n');
}

// Mark unused functions as used
[[maybe_unused]] static void entry_del(Entry *ent) {
    switch (ent->type) {
    case T_ZSET:
        zset_dispose(ent->zset);
        delete ent->zset;
        break;
    }
    delete ent;
}

// Mark unused functions as used
[[maybe_unused]] static void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0 && ent->heap_idx != (size_t)-1) {
        // erase an item from the heap
        // by replacing it with the last item in the array
        size_t pos = ent->heap_idx;
        g_data.heap[pos] = g_data.heap.back();
        g_data.heap.pop_back();
        if (pos < g_data.heap.size()) {
            heap_update(g_data.heap.data(), pos, g_data.heap.size());
        }
        ent->heap_idx = -1;
    } else if (ttl_ms >= 0) {
        size_t pos = ent->heap_idx;
        if (pos == (size_t)-1) {
            // add an new item to the heap
            HeapItem item;
            item.ref = &ent->heap_idx;
            g_data.heap.push_back(item);
            pos = g_data.heap.size() - 1;
        }
        g_data.heap[pos].val = get_monotonic_usec() + (uint64_t)ttl_ms * 1000;
        heap_update(g_data.heap.data(), pos, g_data.heap.size());
    }
}

// Connection handling functions
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;
    }

    fd_set_nb(connfd);
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn->idle_start = get_monotonic_usec();
    dlist_init(&conn->idle_list);
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);
    conn_put(g_data.fd2conn, conn);
    return 0;
}

static void state_req(Conn *conn) {
    // If we're in WAIT state, don't process requests
    if (conn->state == STATE_WAIT) {
        return;
    }

    while (try_fill_buffer(conn)) {
        if (try_one_request(conn)) {
            state_res(conn);
        }
    }
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {
        if (conn->wbuf_sent == conn->wbuf_size) {
            // If we were in WAIT state, we need to process any pending messages
            if (conn->state == STATE_WAIT) {
                process_incoming_messages();
                // Check if we have a response
                int flag;
                MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
                if (flag) {
                    int source;
                    MessageType type;
                    std::string key, value;
                    receive_message(&source, &type, key, value);
                    if (type == MSG_RESPONSE) {
                        // We got a response, prepare to send it to the client
                        std::string out;
                        out_str(out, value);
                        // Copy the response to the write buffer
                        uint32_t wlen = (uint32_t)out.size();
                        memcpy(conn->wbuf, &wlen, 4);
                        memcpy(conn->wbuf + 4, out.data(), out.size());
                        conn->wbuf_size = 4 + wlen;
                        conn->wbuf_sent = 0;
                        conn->state = STATE_RES;
                        return;
                    }
                }
            }
            conn->state = STATE_REQ;
            conn->wbuf_size = 0;
            conn->wbuf_sent = 0;
            return;
        }
    }
}

static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES || conn->state == STATE_WAIT) {
        state_res(conn);
    } else {
        assert(0);
    }
}

static void conn_done(Conn *conn) {
    g_data.fd2conn[conn->fd] = NULL;
    close(conn->fd);
    dlist_detach(&conn->idle_list);
    free(conn);
}

static uint32_t next_timer_ms() {
    uint64_t now_us = get_monotonic_usec();
    uint64_t next_us = (uint64_t)-1;

    // idle timers
    if (!dlist_empty(&g_data.idle_list)) {
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        next_us = next->idle_start + k_idle_timeout_ms * 1000;
    }

    // TTL timers
    if (!g_data.heap.empty() && g_data.heap[0].val < next_us) {
        next_us = g_data.heap[0].val;
    }

    if (next_us == (uint64_t)-1) {
        return 10000; // no timer, the value doesn't matter
    }

    if (next_us <= now_us) {
        // missed?
        return 0;
    }

    return (uint32_t)((next_us - now_us) / 1000);
}

static void process_timers() {
    uint64_t now_us = get_monotonic_usec();
    while (!dlist_empty(&g_data.idle_list)) {
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
        if (next_us >= now_us + 1000) {
            break;
        }
        printf("removing idle connection: %d\n", next->fd);
        conn_done(next);
    }

    while (!g_data.heap.empty() && g_data.heap[0].val < now_us) {
        Entry *ent = container_of(g_data.heap[0].ref, Entry, heap_idx);
        HNode *node = hm_pop(&g_data.db, &ent->node, entry_eq);
        assert(node == &ent->node);
        entry_del(ent);
    }
}

// Buffer handling functions
static bool try_fill_buffer(Conn *conn) {
    // If we're in WAIT state, don't try to read from the socket
    if (conn->state == STATE_WAIT) {
        return false;
    }

    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);
    
    if (rv < 0 && errno == EAGAIN) {
        return false;
    }
    
    if (rv < 0) {
        msg("read() error");
        conn_done(conn);
        return false;
    }
    
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn_done(conn);
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));
    
    // Update idle timer
    conn->idle_start = get_monotonic_usec();
    return true;
}

static bool try_one_request(Conn *conn) {
    if (conn->rbuf_size < 4) {
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn_done(conn);
        return false;
    }
    if (4 + len > conn->rbuf_size) {
        return false;
    }

    std::vector<std::string> cmd;
    if (0 != parse_req(&conn->rbuf[4], len, cmd)) {
        msg("bad req");
        conn_done(conn);
        return false;
    }

    std::string out;
    if (cmd.size() == 2 && cmd[0] == "get") {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        do_del(cmd, out);
    } else {
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }

    if (4 + out.size() > k_max_msg) {
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }

    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen;

    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;
    return true;
}

static bool try_flush_buffer(Conn *conn) {
    if (conn->wbuf_size == 0) {
        return false;
    }
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn_done(conn);
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    return true;
}

// Request parsing function
static int parse_req(const uint8_t *data, size_t len, std::vector<std::string> &out) {
    if (len < 4) {
        return -1;
    }
    uint32_t n = 0;
    memcpy(&n, data, 4);
    if (n > k_max_msg) {
        return -1;
    }

    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) {
            return -1;
        }
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) {
        return -1;
    }
    return 0;
}

// Hash map initialization
static void hm_init(HMap *hmap) {
    hmap->ht1.tab = NULL;
    hmap->ht1.mask = 0;
    hmap->ht1.size = 0;
    hmap->ht2.tab = NULL;
    hmap->ht2.mask = 0;
    hmap->ht2.size = 0;
    hmap->resizing_pos = 0;
}

int main(int argc, char **argv) {
    // Initialize MPI
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &g_data.rank);
    MPI_Comm_size(MPI_COMM_WORLD, &g_data.size);
    
    // Initialize consistent hashing
    init_consistent_hashing();
    
    // Initialize the database
    hm_init(&g_data.db);
    
    // Initialize DList members
    dlist_init(&g_data.idle_list);
    
    // Set up server socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234 + g_data.rank);  // Each node gets a different port
    addr.sin_addr.s_addr = ntohl(0);
    
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }
    
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }
    
    // Set up polling
    std::vector<struct pollfd> poll_args;
    struct pollfd pfd = {fd, POLLIN, 0};
    poll_args.push_back(pfd);
    
    // Main event loop
    while (true) {
        // Process incoming messages from other nodes
        process_incoming_messages();
        
        // Check for new connections and I/O
        int timeout_ms = next_timer_ms();
        int nevents = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (nevents < 0) {
            die("poll");
        }
        
        // Process timers
        process_timers();
        
        // Handle new connections
        if (poll_args[0].revents) {
            accept_new_conn(fd);
        }
        
        // Handle client connections
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = g_data.fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    conn_done(conn);
                    poll_args.erase(poll_args.begin() + i);
                    --i;
                }
            }
        }
    }
    
    MPI_Finalize();
    return 0;
} 