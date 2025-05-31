//client
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <windows.h>
#include <signal.h>

#define MAX_LEN 200
#define NUM_COLORS 1
#define SERVER_PASSWORD "WCHAT5"

#pragma comment(lib, "ws2_32.lib")

int exit_flag = 0;
HANDLE t_send = NULL;
HANDLE t_recv = NULL;
SOCKET client_socket = INVALID_SOCKET;
CRITICAL_SECTION cs_print;
int in_private_chat = 0;
char pending_request_from[MAX_LEN] = { 0 };
int pending_request_color = 0;
int my_color_code = 0;

const char* colors[] = { "\033[36m" };
const char* def_col = "\033[0m";

void ProcessListCommand() {
    char command[] = "/list";
    send(client_socket, command, (int)strlen(command) + 1, 0);
}

void catch_ctrl_c(int signal) {
    (void)signal;
    if (exit_flag) return;

    exit_flag = 1;
    const char* exit_msg = "#exit";
    if (client_socket != INVALID_SOCKET) {
        send(client_socket, exit_msg, (int)strlen(exit_msg) + 1, 0);
        closesocket(client_socket);
        client_socket = INVALID_SOCKET;
    }
}

const char* color(int code) {
    return colors[code % NUM_COLORS];
}

void eraseText(int cnt) {
    EnterCriticalSection(&cs_print);
    for (int i = 0; i < cnt; i++) {
        printf("\b \b");
    }
    LeaveCriticalSection(&cs_print);
}

unsigned __stdcall send_message(void* params) {
    (void)params;
    char str[MAX_LEN] = { 0 };

    while (!exit_flag) {
        EnterCriticalSection(&cs_print);
        printf("%sYou: %s", colors[0], def_col);
        LeaveCriticalSection(&cs_print);

        if (fgets(str, MAX_LEN, stdin) == NULL) {
            break;
        }
        str[strcspn(str, "\n")] = '\0';

        if ((strcmp(str, "Y") == 0 || strcmp(str, "N") == 0) && strlen(pending_request_from) > 0) {
            send(client_socket, str, (int)strlen(str) + 1, 0);
            memset(pending_request_from, 0, sizeof(pending_request_from));
            continue;
        }

        if (in_private_chat && strcmp(str, "return") == 0) {
            send(client_socket, "#endprivate", (int)strlen("#endprivate") + 1, 0);
            in_private_chat = 0;
            continue;
        }

        if (strcmp(str, "/help") == 0) {
            EnterCriticalSection(&cs_print);
            printf("\nCommands:\n");
            printf("/<username> - Start a private chat with that user\n");
            printf("return - Leave private chat and return to group chat\n");
            printf("//<username> <message> - Send a direct message\n");
            printf("/ai <message> - Chat with Gemini AI\n");
            printf("/list - List users currently in public chat\n"); // Updated description
            printf("/help - Show this help\n");
            printf("#exit - Disconnect from server\n");
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
            LeaveCriticalSection(&cs_print);
            continue;
        }

        if (strcmp(str, "/list") == 0) {
            ProcessListCommand();
            continue;
        }

        if (strncmp(str, "//", 2) == 0) {
            char* target_name = str + 2;
            char* message = strchr(target_name, ' ');
            if (message) {
                *message = '\0';
                message++;
                if (strlen(message) > 0) {
                    char direct_msg[MAX_LEN];
                    snprintf(direct_msg, sizeof(direct_msg), "//%s %s", target_name, message);
                    send(client_socket, direct_msg, (int)strlen(direct_msg) + 1, 0);
                }
            }
            continue;
        }

        if (send(client_socket, str, (int)strlen(str) + 1, 0) == SOCKET_ERROR) {
            printf("Send failed: %d\n", WSAGetLastError());
            break;
        }

        if (strcmp(str, "#exit") == 0) {
            exit_flag = 1;
            break;
        }
    }
    _endthreadex(0);
    return 0;
}

unsigned __stdcall recv_message(void* params) {
    (void)params;
    char str[MAX_LEN] = { 0 };
    char* context = NULL;

    while (!exit_flag) {
        int bytes_received = recv(client_socket, str, sizeof(str), 0);
        if (bytes_received <= 0 || exit_flag) {
            break;
        }

        EnterCriticalSection(&cs_print);

        if (strstr(str, "PRIVATE_REQUEST:") != NULL) {
            char* requester_name = str + strlen("PRIVATE_REQUEST:");
            char* color_ptr = strchr(requester_name, ':') + 1;
            int req_color = atoi(color_ptr);
            char* token = strtok_s(requester_name, ":", &context);

            strcpy_s(pending_request_from, sizeof(pending_request_from), token);
            pending_request_color = req_color;

            eraseText(6);
            printf("\n%s%s wants to start a private chat with you. (Y/N): %s",
                colors[0], token, def_col);
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
        }
        else if (strstr(str, "PRIVATE_CHAT_STARTED:") != NULL) {
            char* partner_name = str + strlen("PRIVATE_CHAT_STARTED:");
            char* color_ptr = strchr(partner_name, ':') + 1;
            int partner_color = atoi(color_ptr);
            char* token = strtok_s(partner_name, ":", &context);

            in_private_chat = 1;
            eraseText(6);
            printf("\nNow in private chat with %s%s%s\n", colors[0], token, def_col);
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
        }
        else if (strstr(str, "PRIVATE_REQUEST_REJECTED:") != NULL) {
            char* rejecter_name = str + strlen("PRIVATE_REQUEST_REJECTED:");
            char* color_ptr = strchr(rejecter_name, ':') + 1;
            int rejecter_color = atoi(color_ptr);
            char* token = strtok_s(rejecter_name, ":", &context);

            eraseText(6);
            printf("\n%s%s rejected your private chat request%s\n",
                colors[0], token, def_col);
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
        }
        else if (strcmp(str, "PRIVATE_CHAT_ENDED") == 0) {
            in_private_chat = 0;
            eraseText(6);
            printf("\nPrivate chat ended. Back to group chat\n");
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
        }
        else if (strstr(str, "PRIVATE_MSG:") != NULL) {
            char* sender_name = str + strlen("PRIVATE_MSG:");
            char* color_ptr = strchr(sender_name, ':') + 1;
            int sender_color = atoi(color_ptr);
            char* message = strchr(color_ptr, ':') + 1;
            char* token = strtok_s(sender_name, ":", &context);

            eraseText(6);
            printf("\n%s[Private] %s: %s%s\n", colors[0], token, def_col, message);
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
        }
        else if (strstr(str, "DIRECT_MSG:") != NULL) {
            char* sender_name = str + strlen("DIRECT_MSG:");
            char* color_ptr = strchr(sender_name, ':') + 1;
            int sender_color = atoi(color_ptr);
            char* message = strchr(color_ptr, ':') + 1;
            char* token = strtok_s(sender_name, ":", &context);

            eraseText(6);
            printf("\n%s[Direct] %s: %s%s\n", colors[0], token, def_col, message);
            printf("%sYou: %s", colors[0], def_col);
            fflush(stdout);
        }
        else if (strstr(str, "GROUP_MSG:") != NULL) {
            if (!in_private_chat) {
                char* sender_name = str + strlen("GROUP_MSG:");
                char* color_ptr = strchr(sender_name, ':') + 1;
                int sender_color = atoi(color_ptr);
                char* message = strchr(color_ptr, ':') + 1;
                char* token = strtok_s(sender_name, ":", &context);

                eraseText(6);
                printf("\n%s%s: %s%s\n", colors[0], token, def_col, message);
                printf("%sYou: %s", colors[0], def_col);
                fflush(stdout);
            }
        }
        else if (strstr(str, "has left the group chat\n") != NULL ||
            strstr(str, "has rejoined the group chat\n") != NULL ||
            strstr(str, "has joined") != NULL) {
            if (!in_private_chat) {
                eraseText(6);
                printf("\n%s\n", str);
                printf("%sYou: %s", colors[0], def_col);
                fflush(stdout);
            }
        }
        else if (strstr(str, "COLOR_ASSIGN:") != NULL) {
            my_color_code = atoi(str + strlen("COLOR_ASSIGN:"));
        }
        else {
            if (!in_private_chat) {
                eraseText(6);
                printf("\n%s\n", str);
                printf("%sYou: %s", colors[0], def_col);
                fflush(stdout);
            }
        }

        LeaveCriticalSection(&cs_print);
    }

    if (!exit_flag) {
        exit_flag = 1;
        catch_ctrl_c(0);
    }

    _endthreadex(0);
    return 0;
}

int main() {
    WSADATA wsa;
    struct sockaddr_in server;
    char password[MAX_LEN] = { 0 };
    char name[MAX_LEN] = { 0 };
    char buffer[MAX_LEN] = { 0 };

    printf("Loading Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Could not create socket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(8888);
    if (inet_pton(AF_INET, "10.67.164.217", &server.sin_addr) <= 0) {
        printf("Invalid address/Address not supported\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    if (connect(client_socket, (struct sockaddr*)&server, sizeof(server)) < 0) {
        printf("Connect error: %d\n", WSAGetLastError());
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    // Send password
    printf("Enter server password: ");
    if (fgets(password, MAX_LEN, stdin) == NULL) {
        printf("Error reading password\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    password[strcspn(password, "\n")] = '\0';

    if (send(client_socket, password, (int)strlen(password) + 1, 0) == SOCKET_ERROR) {
        printf("Send failed: %d\n", WSAGetLastError());
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    // Receive server response
    int bytes_received = recv(client_socket, buffer, MAX_LEN, 0);
    if (bytes_received <= 0) {
        printf("Server disconnected or error: %d\n", WSAGetLastError());
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    buffer[bytes_received] = '\0';

    if (strstr(buffer, "Invalid password") != NULL) {
        printf("%s\n", buffer);
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    else if (strstr(buffer, "PASSWORD_ACCEPTED") == NULL) {
        printf("Unexpected server response: %s\n", buffer);
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    InitializeCriticalSection(&cs_print);

    // Proceed to name input
    printf("Enter your name: ");
    if (fgets(name, MAX_LEN, stdin) == NULL) {
        printf("Error reading name\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    name[strcspn(name, "\n")] = '\0';

    if (send(client_socket, name, (int)strlen(name) + 1, 0) == SOCKET_ERROR) {
        printf("Send failed: %d\n", WSAGetLastError());
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    // Check for name error (e.g., name already in use)
    bytes_received = recv(client_socket, buffer, MAX_LEN, 0);
    if (bytes_received <= 0) {
        printf("Server disconnected or error: %d\n", WSAGetLastError());
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    buffer[bytes_received] = '\0';

    if (strstr(buffer, "Name already in use") != NULL) {
        printf("%s\n", buffer);
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    printf("%s\n \n\t              Welcome to WChat              %s\n", colors[0], def_col);
    printf("\nType /<username> to start a private chat with that user\n");
    printf("Type 'return' to leave private chat and return to group chat\n");
    printf("Type //<username> <message> to send a direct message\n");
    printf("Type /ai <message> to chat with Gemini AI\n");
    printf("Type /list to list users currently in public chat\n"); // Updated description
    printf("Type /help to show commands\n");
    printf("Type #exit to disconnect from server\n \n");

    signal(SIGINT, catch_ctrl_c);

    unsigned threadID;
    t_send = (HANDLE)_beginthreadex(NULL, 0, send_message, NULL, 0, &threadID);
    if (t_send == NULL) {
        printf("Failed to create send thread\n");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    t_recv = (HANDLE)_beginthreadex(NULL, 0, recv_message, NULL, 0, &threadID);
    if (t_recv == NULL) {
        printf("Failed to create receive thread\n");
        CloseHandle(t_send);
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    HANDLE threads[2] = { t_send, t_recv };
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);

    CloseHandle(t_send);
    CloseHandle(t_recv);
    closesocket(client_socket);
    WSACleanup();
    DeleteCriticalSection(&cs_print);
    return 0;
}
