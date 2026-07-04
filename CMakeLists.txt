cmake_minimum_required(VERSION 3.20)
project(chat_room)
set(CMAKE_CXX_STANDARD 17)
enable_testing()

find_package(Threads REQUIRED)

set(LIB_SOURCES
    src/common/buffer.cpp
    src/protocol/packet.cpp
    src/server/reactor.cpp
    src/tcp/connection.cpp
    src/tcp/user_manager.cpp
    src/tcp/message_handler.cpp
    src/tcp/connection_manager.cpp
    src/server/message.cpp
    src/server/http_request.cpp
    src/server/http_response.cpp
    src/server/http_connection.cpp
    src/server/http_server.cpp
    src/server/room.cpp
    src/server/token_store.cpp
    src/server/http_handler.cpp
    # ===== 阶段 3:WebSocket =====
    src/server/ws_frame.cpp
    src/server/ws_handshake.cpp
    src/server/ws_connection.cpp
    src/server/ws_hub.cpp
)
add_library(chat_src STATIC ${LIB_SOURCES})
target_include_directories(chat_src PUBLIC src third_party)
target_compile_options(chat_src PRIVATE -Wall -O2)
target_link_libraries(chat_src PUBLIC Threads::Threads)

add_executable(chat_server src/server/main.cpp)
target_link_libraries(chat_server PRIVATE chat_src)

set(UNIT_TESTS test_buffer test_packet test_reactor test_connection
                 test_user_manager test_connection_manager test_message_handler
                 test_http_request test_http_response test_http_connection
                 test_room
                 # ===== 阶段 3 =====
                 test_ws_frame test_ws_handshake test_ws_connection)
foreach(t ${UNIT_TESTS})
    add_executable(${t} tests/unit/${t}.cpp)
    target_link_libraries(${t} PRIVATE chat_src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()