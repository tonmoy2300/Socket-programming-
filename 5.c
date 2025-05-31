//server
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <windows.h>
#include <time.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define MAX_CLIENTS 100
#define MAX_LEN 200
#define NUM_COLORS 6
#define SERVER_PASSWORD "WCHAT5"
#define GEMINI_API_KEY "AIzaSyCcdhOA_O_7qVHtbsAQ1y7Jhde2hHMtEwI"
#define GEMINI_API_URL "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" GEMINI_API_KEY

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "libcurl.lib")

typedef struct {
    char* memory;
    size_t size;
} MemoryStruct;

typedef struct {
    SOCKET sock;
    char name[MAX_LEN];
    int color_code;
    int in_private_chat;
    char private_partner[MAX_LEN];
    char pending_request_from[MAX_LEN];
} client_t;

client_t* clients[MAX_CLIENTS];
CRITICAL_SECTION cs_clients;
int client_count = 0;

// Modified list_online_users to exclude users in private chat
void list_online_users(SOCKET requesterSocket) {
    char userList[MAX_LEN * 2] = "[Server]: Online users:\n";
    EnterCriticalSection(&cs_clients);

    for (int i = 0; i < client_count; ++i) {
        // Only include users who are not in a private chat
        if (!clients[i]->in_private_chat) {
            strcat_s(userList, sizeof(userList), " - ");
            strcat_s(userList, sizeof(userList), clients[i]->name);
            strcat_s(userList, sizeof(userList), "\n");
        }
    }

    LeaveCriticalSection(&cs_clients);
    send(requesterSocket, userList, (int)strlen(userList) + 1, 0);
}

size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    MemoryStruct* mem = (MemoryStruct*)userp;

    char* ptr = (char*)realloc(mem->memory, mem->size + totalSize + 1);
    if (ptr == NULL) {
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, totalSize);
    mem->size += totalSize;
    mem->memory[mem->size] = 0;
    return totalSize;
}

void getGeminiResponse(const char* message, char* response, size_t response_size) {
    CURL* curl;
    CURLcode res;

    MemoryStruct chunk;
    chunk.memory = (char*)malloc(1);
    chunk.size = 0;

    char postFields[MAX_LEN * 2];
    snprintf(postFields, sizeof(postFields),
        "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}", message);

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, GEMINI_API_URL);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, (struct curl_slist*)curl_slist_append(NULL, "Content-Type: application/json"));

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            snprintf(response, response_size, "Gemini AI Error: %s", curl_easy_strerror(res));
        }
        else {
            cJSON* json = cJSON_Parse(chunk.memory);
            if (json) {
                cJSON* candidates = cJSON_GetObjectItem(json, "candidates");
                if (candidates && cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
                    cJSON* firstCandidate = cJSON_GetArrayItem(candidates, 0);
                    if (firstCandidate) {
                        cJSON* content = cJSON_GetObjectItem(firstCandidate, "content");
                        if (content) {
                            cJSON* parts = cJSON_GetObjectItem(content, "parts");
                            if (parts && cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                                cJSON* firstPart = cJSON_GetArrayItem(parts, 0);
                                if (firstPart) {
                                    cJSON* text = cJSON_GetObjectItem(firstPart, "text");
                                    if (text && cJSON_IsString(text)) {
                                        snprintf(response, response_size, "AI: %s", text->valuestring);
                                    }
                                    else {
                                        snprintf(response, response_size, "Gemini AI Error: No text in response");
                                    }
                                }
                                else {
                                    snprintf(response, response_size, "Gemini AI Error: No parts in response");
                                }
                            }
                            else {
                                snprintf(response, response_size, "Gemini AI Error: No parts array");
                            }
                        }
                        else {
                            snprintf(response, response_size, "Gemini AI Error: No content in response");
                        }
                    }
                    else {
                        snprintf(response, response_size, "Gemini AI Error: No candidates in response");
                    }
                }
                else {
                    snprintf(response, response_size, "Gemini AI Error: No candidates array");
                }
                cJSON_Delete(json);
            }
            else {
                snprintf(response, response_size, "Gemini AI Error: Failed to parse response");
            }
        }

        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
    else {
        snprintf(response, response_size, "Gemini AI Error: Failed to initialize curl");
    }
    curl_global_cleanup();
}

void broadcast_message(char* message, SOCKET sender_sock) {
    EnterCriticalSection(&cs_clients);

    for (int i = 0; i < client_count; i++) {
        if (clients[i]->sock != sender_sock && !clients[i]->in_private_chat) {
            send(clients[i]->sock, message, (int)strlen(message) + 1, 0);
        }
    }

    LeaveCriticalSection(&cs_clients);
}

void send_private_message(char* message, char* sender_name, char* receiver_name, int sender_color) {
    EnterCriticalSection(&cs_clients);

    char private_msg[MAX_LEN * 2];
    snprintf(private_msg, sizeof(private_msg), "PRIVATE_MSG:%s:%d:%s", sender_name, sender_color, message);

    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i]->name, receiver_name) == 0) {
            send(clients[i]->sock, private_msg, (int)strlen(private_msg) + 1, 0);
            break;
        }
    }

    LeaveCriticalSection(&cs_clients);
}

void send_direct_message(char* message, char* sender_name, char* receiver_name, int sender_color, SOCKET sender_sock) {
    EnterCriticalSection(&cs_clients);

    int target_idx = -1;
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i]->name, receiver_name) == 0) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == -1) {
        char error_msg[MAX_LEN];
        snprintf(error_msg, sizeof(error_msg), "User '%s' not found or not available", receiver_name);
        send(sender_sock, error_msg, (int)strlen(error_msg) + 1, 0);
        LeaveCriticalSection(&cs_clients);
        return;
    }

    char direct_msg[MAX_LEN * 2];
    snprintf(direct_msg, sizeof(direct_msg), "DIRECT_MSG:%s:%d:%s", sender_name, sender_color, message);

    send(clients[target_idx]->sock, direct_msg, (int)strlen(direct_msg) + 1, 0);

    LeaveCriticalSection(&cs_clients);
}

int find_client_by_socket(SOCKET sock) {
    EnterCriticalSection(&cs_clients);
    int index = -1;

    for (int i = 0; i < client_count; i++) {
        if (clients[i]->sock == sock) {
            index = i;
            break;
        }
    }

    LeaveCriticalSection(&cs_clients);
    return index;
}

int find_client_by_name(char* name) {
    EnterCriticalSection(&cs_clients);
    int index = -1;

    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i]->name, name) == 0) {
            index = i;
            break;
        }
    }

    LeaveCriticalSection(&cs_clients);
    return index;
}

void handle_private_chat_request(SOCKET requester_sock, char* target_name) {
    EnterCriticalSection(&cs_clients);

    int requester_idx = find_client_by_socket(requester_sock);
    int target_idx = find_client_by_name(target_name);

    if (requester_idx == -1 || target_idx == -1) {
        LeaveCriticalSection(&cs_clients);
        char error_msg[MAX_LEN];
        snprintf(error_msg, sizeof(error_msg), "User '%s' not found or not available", target_name);
        send(requester_sock, error_msg, (int)strlen(error_msg) + 1, 0);
        return;
    }

    if (clients[target_idx]->in_private_chat) {
        LeaveCriticalSection(&cs_clients);
        char busy_msg[MAX_LEN];
        snprintf(busy_msg, sizeof(busy_msg), "User '%s' is already in a private chat", target_name);
        send(requester_sock, busy_msg, (int)strlen(busy_msg) + 1, 0);
        return;
    }

    char request_msg[MAX_LEN];
    snprintf(request_msg, sizeof(request_msg), "PRIVATE_REQUEST:%s:%d",
        clients[requester_idx]->name, clients[requester_idx]->color_code);

    strncpy(clients[target_idx]->pending_request_from, clients[requester_idx]->name, MAX_LEN - 1);

    send(clients[target_idx]->sock, request_msg, (int)strlen(request_msg) + 1, 0);

    LeaveCriticalSection(&cs_clients);
}

void start_private_chat(SOCKET responder_sock, char* response) {
    EnterCriticalSection(&cs_clients);

    int responder_idx = find_client_by_socket(responder_sock);
    if (responder_idx == -1) {
        LeaveCriticalSection(&cs_clients);
        return;
    }

    char request_sender[MAX_LEN] = { 0 };
    strncpy(request_sender, clients[responder_idx]->pending_request_from, MAX_LEN - 1);

    int request_sender_idx = find_client_by_name(request_sender);
    if (request_sender_idx == -1) {
        LeaveCriticalSection(&cs_clients);
        char error_msg[MAX_LEN];
        snprintf(error_msg, sizeof(error_msg), "The requesting user is no longer available");
        send(responder_sock, error_msg, (int)strlen(error_msg) + 1, 0);
        return;
    }

    if (strcmp(response, "Y") == 0) {
        clients[responder_idx]->in_private_chat = 1;
        clients[request_sender_idx]->in_private_chat = 1;

        strncpy(clients[responder_idx]->private_partner, request_sender, MAX_LEN - 1);
        strncpy(clients[request_sender_idx]->private_partner, clients[responder_idx]->name, MAX_LEN - 1);

        char start_msg_to_responder[MAX_LEN];
        snprintf(start_msg_to_responder, sizeof(start_msg_to_responder), "PRIVATE_CHAT_STARTED:%s:%d",
            request_sender, clients[request_sender_idx]->color_code);

        char start_msg_to_requester[MAX_LEN];
        snprintf(start_msg_to_requester, sizeof(start_msg_to_requester), "PRIVATE_CHAT_STARTED:%s:%d",
            clients[responder_idx]->name, clients[responder_idx]->color_code);

        send(responder_sock, start_msg_to_responder, (int)strlen(start_msg_to_responder) + 1, 0);
        send(clients[request_sender_idx]->sock, start_msg_to_requester, (int)strlen(start_msg_to_requester) + 1, 0);

        char broadcast_msg1[MAX_LEN * 2];
        snprintf(broadcast_msg1, sizeof(broadcast_msg1), "%s has left the group chat",
            clients[request_sender_idx]->name);
        broadcast_message(broadcast_msg1, INVALID_SOCKET);

        char broadcast_msg2[MAX_LEN * 2];
        snprintf(broadcast_msg2, sizeof(broadcast_msg2), "%s has left the group chat",
            clients[responder_idx]->name);
        broadcast_message(broadcast_msg2, INVALID_SOCKET);
    }
    else {
        char reject_msg[MAX_LEN];
        snprintf(reject_msg, sizeof(reject_msg), "PRIVATE_REQUEST_REJECTED:%s:%d",
            clients[responder_idx]->name, clients[responder_idx]->color_code);

        send(clients[request_sender_idx]->sock, reject_msg, (int)strlen(reject_msg) + 1, 0);
    }

    memset(clients[responder_idx]->pending_request_from, 0, MAX_LEN);
    LeaveCriticalSection(&cs_clients);
}

void end_private_chat(SOCKET sock) {
    EnterCriticalSection(&cs_clients);

    int client_idx = find_client_by_socket(sock);
    if (client_idx == -1 || !clients[client_idx]->in_private_chat) {
        LeaveCriticalSection(&cs_clients);
        return;
    }

    char partner_name[MAX_LEN];
    strncpy(partner_name, clients[client_idx]->private_partner, MAX_LEN - 1);

    int partner_idx = find_client_by_name(partner_name);

    clients[client_idx]->in_private_chat = 0;
    memset(clients[client_idx]->private_partner, 0, MAX_LEN);

    if (partner_idx != -1) {
        clients[partner_idx]->in_private_chat = 0;
        memset(clients[partner_idx]->private_partner, 0, MAX_LEN);

        const char* end_msg = "PRIVATE_CHAT_ENDED";
        send(clients[partner_idx]->sock, end_msg, (int)strlen(end_msg) + 1, 0);
    }

    const char* end_msg = "PRIVATE_CHAT_ENDED";
    send(clients[client_idx]->sock, end_msg, (int)strlen(end_msg) + 1, 0);

    char broadcast_msg1[MAX_LEN * 2];
    snprintf(broadcast_msg1, sizeof(broadcast_msg1), "%s has rejoined the group chat\n",
        clients[client_idx]->name);
    broadcast_message(broadcast_msg1, INVALID_SOCKET);

    if (partner_idx != -1) {
        char broadcast_msg2[MAX_LEN * 2];
        snprintf(broadcast_msg2, sizeof(broadcast_msg2), "%s has rejoined the group chat\n",
            clients[partner_idx]->name);
        broadcast_message(broadcast_msg2, INVALID_SOCKET);
    }

    LeaveCriticalSection(&cs_clients);
}

unsigned __stdcall handle_client(void* arg) {
    SOCKET client_sock = *(SOCKET*)arg;
    free(arg);

    // Receive and verify password
    char password[MAX_LEN] = { 0 };
    int received = recv(client_sock, password, MAX_LEN, 0);
    if (received <= 0) {
        closesocket(client_sock);
        return 1;
    }

    if (strcmp(password, SERVER_PASSWORD) != 0) {
        const char* error_msg = "Invalid password";
        send(client_sock, error_msg, (int)strlen(error_msg) + 1, 0);
        closesocket(client_sock);
        return 1;
    }

    // Send password acceptance
    const char* accept_msg = "PASSWORD_ACCEPTED";
    send(client_sock, accept_msg, (int)strlen(accept_msg) + 1, 0);

    // Receive client name
    char name[MAX_LEN] = { 0 };
    received = recv(client_sock, name, MAX_LEN, 0);
    if (received <= 0) {
        closesocket(client_sock);
        return 1;
    }

    EnterCriticalSection(&cs_clients);

    int name_exists = 0;
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i]->name, name) == 0) {
            name_exists = 1;
            break;
        }
    }

    if (name_exists) {
        LeaveCriticalSection(&cs_clients);
        const char* error_msg = "Name already in use. Please reconnect with a different name.";
        send(client_sock, error_msg, (int)strlen(error_msg) + 1, 0);
        closesocket(client_sock);
        return 1;
    }

    client_t* new_client = (client_t*)malloc(sizeof(client_t));
    if (!new_client) {
        LeaveCriticalSection(&cs_clients);
        const char* error_msg = "Server error: Could not allocate memory for client.";
        send(client_sock, error_msg, (int)strlen(error_msg) + 1, 0);
        closesocket(client_sock);
        return 1;
    }

    new_client->sock = client_sock;
    strncpy(new_client->name, name, MAX_LEN - 1);
    new_client->color_code = rand() % NUM_COLORS;
    new_client->in_private_chat = 0;
    memset(new_client->private_partner, 0, MAX_LEN);
    memset(new_client->pending_request_from, 0, MAX_LEN);

    char color_msg[MAX_LEN];
    snprintf(color_msg, sizeof(color_msg), "COLOR_ASSIGN:%d", new_client->color_code);
    send(client_sock, color_msg, (int)strlen(color_msg) + 1, 0);

    clients[client_count++] = new_client;

    char join_msg[MAX_LEN * 2];
    snprintf(join_msg, sizeof(join_msg), "%s has joined the group chat\n", name);

    LeaveCriticalSection(&cs_clients);

    broadcast_message(join_msg, client_sock);

    char buffer[MAX_LEN] = { 0 };
    while (1) {
        received = recv(client_sock, buffer, MAX_LEN, 0);
        if (received <= 0) {
            break;
        }

        if (strcmp(buffer, "#exit") == 0) {
            break;
        }

        if (strcmp(buffer, "#endprivate") == 0) {
            end_private_chat(client_sock);
            continue;
        }

        if ((strcmp(buffer, "Y") == 0 || strcmp(buffer, "N") == 0)) {
            int client_idx = find_client_by_socket(client_sock);
            if (client_idx != -1 && strlen(clients[client_idx]->pending_request_from) > 0) {
                start_private_chat(client_sock, buffer);
                continue;
            }
        }

        int client_idx = find_client_by_socket(client_sock);
        if (client_idx == -1) {
            continue;
        }

        if (strcmp(buffer, "/list") == 0) {
            list_online_users(client_sock);
            continue;
        }

        if (strncmp(buffer, "/ai", 3) == 0) {
            if (strlen(buffer) > 4) {
                char aiResponse[MAX_LEN * 2];
                getGeminiResponse(buffer + 4, aiResponse, sizeof(aiResponse));
                send(client_sock, aiResponse, (int)strlen(aiResponse) + 1, 0);
            }
            else {
                const char* usage_msg = "Usage: /ai <your question>";
                send(client_sock, usage_msg, (int)strlen(usage_msg) + 1, 0);
            }
            continue;
        }

        if (strncmp(buffer, "//", 2) == 0) {
            char* target_name = buffer + 2;
            char* message = strchr(target_name, ' ');
            if (message) {
                *message = '\0';
                message++;
                if (strlen(message) > 0) {
                    send_direct_message(message, clients[client_idx]->name, target_name,
                        clients[client_idx]->color_code, client_sock);
                }
            }
            continue;
        }

        if (buffer[0] == '/') {
            char target_name[MAX_LEN] = { 0 };
            strncpy(target_name, buffer + 1, sizeof(target_name) - 1);
            handle_private_chat_request(client_sock, target_name);
            continue;
        }

        if (clients[client_idx]->in_private_chat) {
            send_private_message(buffer, clients[client_idx]->name,
                clients[client_idx]->private_partner,
                clients[client_idx]->color_code);
        }
        else {
            char msg[MAX_LEN * 2];
            snprintf(msg, sizeof(msg), "GROUP_MSG:%s:%d:%s",
                clients[client_idx]->name, clients[client_idx]->color_code, buffer);
            broadcast_message(msg, client_sock);
        }
    }

    EnterCriticalSection(&cs_clients);

    int idx = -1;
    for (int i = 0; i < client_count; i++) {
        if (clients[i]->sock == client_sock) {
            idx = i;
            break;
        }
    }

    if (idx != -1) {
        if (clients[idx]->in_private_chat) {
            char partner_name[MAX_LEN];
            strncpy(partner_name, clients[idx]->private_partner, MAX_LEN - 1);

            int partner_idx = -1;
            for (int i = 0; i < client_count; i++) {
                if (strcmp(clients[i]->name, partner_name) == 0) {
                    partner_idx = i;
                    break;
                }
            }

            if (partner_idx != -1) {
                clients[partner_idx]->in_private_chat = 0;
                memset(clients[partner_idx]->private_partner, 0, MAX_LEN);

                const char* end_msg = "PRIVATE_CHAT_ENDED";
                send(clients[partner_idx]->sock, end_msg, (int)strlen(end_msg) + 1, 0);
            }
        }

        char leave_msg[MAX_LEN * 2];
        snprintf(leave_msg, sizeof(leave_msg), "%s has left the group chat", clients[idx]->name);

        free(clients[idx]);
        for (int i = idx; i < client_count - 1; i++) {
            clients[i] = clients[i + 1];
        }
        client_count--;

        LeaveCriticalSection(&cs_clients);

        broadcast_message(leave_msg, INVALID_SOCKET);
    }
    else {
        LeaveCriticalSection(&cs_clients);
    }

    closesocket(client_sock);
    _endthreadex(0);
    return 0;
}

int main() {
    //INITIALIZE er jonno
    WSADATA wsa;
    SOCKET server_sock, client_sock;
    struct sockaddr_in server, client;
    int c = sizeof(struct sockaddr_in);
    HANDLE thread;
    unsigned threadID;

    printf("Loading Winsock...");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }
    printf("Initialized.\n");

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Could not create socket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    printf("Socket created.\n");

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(8888);

    if (bind(server_sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed with error code: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }
    printf("Bind done.\n");

    listen(server_sock, 5);
    printf("Waiting for incoming connections...\n");

    InitializeCriticalSection(&cs_clients);

    srand((unsigned int)time(NULL));

    while (1) {
        client_sock = accept(server_sock, (struct sockaddr*)&client, &c);
        if (client_sock == INVALID_SOCKET) {
            printf("Accept failed with error code: %d\n", WSAGetLastError());
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Connection accepted from %s:%d\n", client_ip, ntohs(client.sin_port));

        SOCKET* new_sock = (SOCKET*)malloc(sizeof(SOCKET));
        if (new_sock == NULL) {
            printf("Memory allocation failed\n");
            closesocket(client_sock);
            continue;
        }
        *new_sock = client_sock;

        thread = (HANDLE)_beginthreadex(NULL, 0, handle_client, (void*)new_sock, 0, &threadID);
        if (thread == NULL) {
            printf("Thread creation failed\n");
            free(new_sock);
            closesocket(client_sock);
            continue;
        }

        CloseHandle(thread);
    }

    closesocket(server_sock);
    WSACleanup();
    DeleteCriticalSection(&cs_clients);

    return 0;
}