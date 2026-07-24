idf_component_register(
    SRCS 
        "../WebSockets/src/WebSockets.cpp" 
        "../WebSockets/src/WebSocketsClient.cpp" 
        "../WebSockets/src/WebSocketsServer.cpp"
    INCLUDE_DIRS "../WebSockets/src"
    REQUIRES arduino-esp32
)