cmake_minimum_required(VERSION 3.10)
project(FluxDB)

# Find required packages
find_package(MPI REQUIRED)
find_package(OpenMP REQUIRED)
find_package(Threads REQUIRED)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include directories
include_directories(include)
include_directories(${MPI_CXX_INCLUDE_PATH})

# Common source files used by both server and client
set(COMMON_SOURCES
    src/hashtable.cpp
    src/zset.cpp
    src/avl.cpp
    src/heap.cpp
    src/thread_pool.cpp
)

# Server-specific source files
set(SERVER_SOURCES
    src/distributed_server.cpp
)

# Client-specific source files
set(CLIENT_SOURCES
    src/client.cpp
)

# Create server executable
add_executable(fluxdb_server ${COMMON_SOURCES} ${SERVER_SOURCES})

# Create client executable
add_executable(fluxdb_client ${COMMON_SOURCES} ${CLIENT_SOURCES})

# Link server against MPI, OpenMP, and Threads
target_link_libraries(fluxdb_server PRIVATE 
    MPI::MPI_CXX 
    OpenMP::OpenMP_CXX
    Threads::Threads
)

# Link client (no MPI/OpenMP needed, but needs Threads)
target_link_libraries(fluxdb_client PRIVATE
    Threads::Threads
)

# Set compiler flags for both executables
foreach(TARGET fluxdb_server fluxdb_client)
    target_compile_options(${TARGET} PRIVATE
        -Wall
        -Wextra
        -Werror
        -O2
    )
endforeach()

# Install targets
install(TARGETS fluxdb_server fluxdb_client DESTINATION bin) 