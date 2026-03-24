#include <gtest/gtest.h>

#pragma comment(lib, "Ws2_32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>

#include "task_host_core/debug.h"

TEST(signal_forge_host_test, Main) {
    // 1. Initialize Winsock
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    ASSERT_EQ(iResult, 0) << "WSAStartup failed";

    // 2. Create the socket
    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSocket == INVALID_SOCKET) {
        WSACleanup();
        FAIL() << "Socket creation failed: " << WSAGetLastError();
    }

    // 3. Setup address structure
    sockaddr_in clientService;
    clientService.sin_family = AF_INET;
    clientService.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &clientService.sin_addr);

    // 4. Set socket to non-blocking mode to implement the timeout
    u_long mode = 1; // 1 to enable non-blocking
    ioctlsocket(connectSocket, FIONBIO, &mode);

    // 5. Start Connection
    connect(connectSocket, (SOCKADDR*)&clientService, sizeof(clientService));

    // 6. Implement WaitOnConnect(500ms) using select()
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(connectSocket, &writeSet);

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000; // 500ms

    // Check if the socket becomes writable (signaling connection success)
    int selectResult = select(0, NULL, &writeSet, NULL, &timeout);

    if (selectResult <= 0) {
        // 0 = timeout, <0 = error
        closesocket(connectSocket);
        WSACleanup();
        if (selectResult == 0) return; // Timeout reached, just exit as per your original logic
        FAIL() << "Select error: " << WSAGetLastError();
    }

    // 7. Set socket back to blocking mode for simple Read/Write logic
    mode = 0;
    ioctlsocket(connectSocket, FIONBIO, &mode);

    // 8. Write data
    DebugRequest req = {
        .pin_id = 5
    };

    int bytesSent = send(connectSocket, (const char*)&req, sizeof(req), 0);
    EXPECT_NE(bytesSent, SOCKET_ERROR);

    // 9. Read data
    char buffer[4096];
    int bytesRead = recv(connectSocket, buffer, sizeof(buffer), 0);
    EXPECT_GT(bytesRead, 0);

    DebugReply* reply = (DebugReply*)buffer;

    // 10. Cleanup
    closesocket(connectSocket);
    WSACleanup();
}