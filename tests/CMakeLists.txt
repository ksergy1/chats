
add_executable(tp-test tp.c)
target_link_libraries(tp-test chats-thread-pool)

add_executable(io-test io.c)
target_link_libraries(io-test chats-io-service chats-thread-pool chats-timer)

add_executable(oto-server-test oto-server.c)
target_link_libraries(oto-server-test chats-io-service
                                      chats-thread-pool
                                      chats-timer
                                      chats-network)

add_executable(client-tcp-test tcp-client.c)
target_link_libraries(client-tcp-test chats-io-service
                                      chats-thread-pool
                                      chats-timer
                                      chats-network)

add_executable(otm-server-test otm-server.c)
target_link_libraries(otm-server-test chats-io-service
                                      chats-thread-pool
                                      chats-timer
                                      chats-network)

add_executable(client-udp-test udp-client.c)
target_link_libraries(client-udp-test chats-io-service
                                      chats-thread-pool
                                      chats-timer
                                      chats-network)
