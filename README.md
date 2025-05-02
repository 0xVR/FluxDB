# FluxDB
![Logo](https://i.imgur.com/f55sFBO.png)

A distributed in-memory key-value database inspired by Redis. Supports basic key-value storage, common Redis commands, and sorted sets with score-based ordering. The program is multithreaded and does some simple networking using sockets.

### Distributed Mode

- Keys are distributed across nodes using consistent hashing
- Each node is responsible for a portion of the hash ring
- Virtual nodes ensure even distribution of keys
- Automatic routing of requests to the correct node
- Non-blocking I/O for better performance
- Fault tolerance through node replication


## Installation

1. Clone the repo

```sh
git clone https://github.com/0xVR/FluxDB.git
```

2. Build the server and client with your preferred compiler. For example:

```sh
cd FluxDB
g++ -o server src/server.cpp
g++ -o client src/client.cpp
```

## Usage

Once the server is running, you can use the provided client to connect to it and send commands. Here are the ones currently supported:

- `SET key value`: Set the value of a key.
- `GET key`: Get the value of a key.
- `DEL key`: Delete a key.
- `KEYS pattern`: Retrieve all keys matching a pattern.
- `ZADD key score member`: Add a member with a score to the sorted set.
- `ZREM key member`: Remove a member from the sorted set.
- `ZQUERY key min max`: Retrieve members from the sorted set with scores in the specified range.
- `PEXPIRE key milliseconds`: Sets a timeout on a specified key. After the timeout, the key will be automatically deleted.
- `PTTL key`: Returns the specified keys remaining time-to-live.

For example:
```sh
./client set a 1
```

## Distributed Mode

FluxDB can be run in distributed mode using MPI for horizontal scaling. In this mode, the database is split across multiple nodes using consistent hashing.

### Building for Distributed Mode

```sh
# Create build directory
mkdir build
cd build

# Configure and build with MPI support
cmake ..
make
```

### Running in Distributed Mode

```sh
# Run with 4 nodes
mpirun -n 4 ./fluxdb_server

# Each node will listen on a different port:
# - Node 0: port 1234
# - Node 1: port 1235
# - Node 2: port 1236
# - Node 3: port 1237
```

### Using the Client with Distributed Mode

```sh
# Connect to any node
./fluxdb_client 127.0.0.1 1234

# The system will automatically route your requests to the correct node
SET key value
GET key
DEL key
```