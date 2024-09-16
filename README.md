# FluxDB
![Logo](https://i.imgur.com/f55sFBO.png)

An in-memory key-value database inspired by Redis. Supports basic key-value storage, common Redis commands, and sorted sets with score-based ordering. The program is multithreaded and does some simple networking using sockets.

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
