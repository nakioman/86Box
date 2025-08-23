/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Simple HTTP server for remote control API.
 *
 * Authors: GitHub Copilot
 *
 *          Copyright 2025 GitHub Copilot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define close closesocket
#define ssize_t int
#endif

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/timer.h>
#include <86box/httpd.h>
#include <86box/fdd.h>

/* HTTP server state */
static struct {
    bool running;
    int port;
    int server_socket;
    thread_t *server_thread;
} httpd_state = {0};

/* Route handlers */
typedef struct httpd_route {
    char *path;
    httpd_handler_t handler;
    struct httpd_route *next;
} httpd_route_t;

static httpd_route_t *routes = NULL;

/* Utility functions */
static void httpd_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("[HTTPD] ");
    vprintf(fmt, args);
    va_end(args);
}

static char* httpd_url_decode(const char *str)
{
    size_t len = strlen(str);
    char *decoded = malloc(len + 1);
    size_t i, j;
    
    for (i = 0, j = 0; i < len; i++, j++) {
        if (str[i] == '%' && i + 2 < len) {
            char hex[3] = {str[i+1], str[i+2], 0};
            decoded[j] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (str[i] == '+') {
            decoded[j] = ' ';
        } else {
            decoded[j] = str[i];
        }
    }
    decoded[j] = 0;
    return decoded;
}

static char* httpd_get_query_param(const char *query, const char *param)
{
    if (!query || !param) return NULL;
    
    char *param_start = strstr(query, param);
    if (!param_start) return NULL;
    
    param_start += strlen(param);
    if (*param_start != '=') return NULL;
    param_start++;
    
    char *param_end = strchr(param_start, '&');
    size_t param_len = param_end ? (size_t)(param_end - param_start) : strlen(param_start);
    
    char *value = malloc(param_len + 1);
    strncpy(value, param_start, param_len);
    value[param_len] = 0;
    
    char *decoded = httpd_url_decode(value);
    free(value);
    return decoded;
}

/* HTTP response functions */
httpd_response_t* httpd_create_response(int status_code, const char *content_type, const char *body)
{
    httpd_response_t *response = malloc(sizeof(httpd_response_t));
    response->status_code = status_code;
    response->content_type = strdup(content_type ? content_type : "text/plain");
    response->body = strdup(body ? body : "");
    response->body_length = strlen(response->body);
    return response;
}

void httpd_free_response(httpd_response_t *response)
{
    if (response) {
        free(response->content_type);
        free(response->body);
        free(response);
    }
}

/* Route registration */
void httpd_register_handler(const char *path, httpd_handler_t handler)
{
    httpd_route_t *route = malloc(sizeof(httpd_route_t));
    route->path = strdup(path);
    route->handler = handler;
    route->next = routes;
    routes = route;
}

static httpd_handler_t httpd_find_handler(const char *path)
{
    httpd_route_t *route = routes;
    while (route) {
        if (strcmp(route->path, path) == 0) {
            return route->handler;
        }
        route = route->next;
    }
    return NULL;
}

/* Built-in API handlers */
httpd_response_t* httpd_api_status(const char *method, const char *path, const char *query, const char *body)
{
    (void)path; (void)query; (void)body;
    
    if (strcmp(method, "GET") != 0) {
        return httpd_create_response(405, "application/json", "{\"error\":\"Method not allowed\"}");
    }
    
    char response_body[512];
    snprintf(response_body, sizeof(response_body), 
        "{\"status\":\"running\",\"version\":\"%s\",\"httpd_port\":%d}", 
        emu_version, httpd_state.port);
    
    return httpd_create_response(200, "application/json", response_body);
}

httpd_response_t* httpd_api_floppy_load(const char *method, const char *path, const char *query, const char *body)
{
    (void)path; (void)body;
    
    if (strcmp(method, "POST") != 0) {
        return httpd_create_response(405, "application/json", "{\"error\":\"Method not allowed\"}");
    }
    
    char *drive_str = httpd_get_query_param(query, "drive");
    char *filename = httpd_get_query_param(query, "filename");
    
    if (!drive_str || !filename) {
        free(drive_str);
        free(filename);
        return httpd_create_response(400, "application/json", 
            "{\"error\":\"Missing required parameters: drive and filename\"}");
    }
    
    int drive = atoi(drive_str);
    if (drive < 0 || drive >= FDD_NUM) {
        free(drive_str);
        free(filename);
        return httpd_create_response(400, "application/json", "{\"error\":\"Invalid drive number\"}");
    }
    
    httpd_log("Loading floppy image '%s' into drive %d\n", filename, drive);
    fdd_load(drive, filename);
    
    char response_body[256];
    snprintf(response_body, sizeof(response_body), 
        "{\"success\":true,\"drive\":%d,\"filename\":\"%s\"}", drive, filename);
    
    free(drive_str);
    free(filename);
    
    return httpd_create_response(200, "application/json", response_body);
}

httpd_response_t* httpd_api_floppy_eject(const char *method, const char *path, const char *query, const char *body)
{
    (void)path; (void)body;
    
    if (strcmp(method, "POST") != 0) {
        return httpd_create_response(405, "application/json", "{\"error\":\"Method not allowed\"}");
    }
    
    char *drive_str = httpd_get_query_param(query, "drive");
    
    if (!drive_str) {
        return httpd_create_response(400, "application/json", 
            "{\"error\":\"Missing required parameter: drive\"}");
    }
    
    int drive = atoi(drive_str);
    if (drive < 0 || drive >= FDD_NUM) {
        free(drive_str);
        return httpd_create_response(400, "application/json", "{\"error\":\"Invalid drive number\"}");
    }
    
    httpd_log("Ejecting floppy from drive %d\n", drive);
    fdd_close(drive);
    
    char response_body[128];
    snprintf(response_body, sizeof(response_body), 
        "{\"success\":true,\"drive\":%d}", drive);
    
    free(drive_str);
    
    return httpd_create_response(200, "application/json", response_body);
}

/* HTTP request parsing and handling */
static void httpd_send_response(int client_socket, httpd_response_t *response)
{
    char status_text[64];
    switch (response->status_code) {
        case 200: strcpy(status_text, "OK"); break;
        case 400: strcpy(status_text, "Bad Request"); break;
        case 404: strcpy(status_text, "Not Found"); break;
        case 405: strcpy(status_text, "Method Not Allowed"); break;
        case 500: strcpy(status_text, "Internal Server Error"); break;
        default: strcpy(status_text, "Unknown"); break;
    }
    
    char header[1024];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        response->status_code, status_text, response->content_type, response->body_length);
    
    send(client_socket, header, strlen(header), 0);
    if (response->body_length > 0) {
        send(client_socket, response->body, response->body_length, 0);
    }
}

static void httpd_handle_request(int client_socket)
{
    char buffer[HTTPD_BUFFER_SIZE];
    ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received <= 0) {
        close(client_socket);
        return;
    }
    
    buffer[bytes_received] = 0;
    
    // Parse HTTP request line
    char method[16], path[256], version[16];
    if (sscanf(buffer, "%15s %255s %15s", method, path, version) != 3) {
        httpd_response_t *response = httpd_create_response(400, "text/plain", "Bad Request");
        httpd_send_response(client_socket, response);
        httpd_free_response(response);
        close(client_socket);
        return;
    }
    
    // Handle CORS preflight
    if (strcmp(method, "OPTIONS") == 0) {
        httpd_response_t *response = httpd_create_response(200, "text/plain", "");
        httpd_send_response(client_socket, response);
        httpd_free_response(response);
        close(client_socket);
        return;
    }
    
    // Extract query parameters
    char *query = strchr(path, '?');
    if (query) {
        *query++ = 0;
    }
    
    // Find and execute handler
    httpd_handler_t handler = httpd_find_handler(path);
    httpd_response_t *response;
    
    if (handler) {
        response = handler(method, path, query, NULL);
    } else {
        response = httpd_create_response(404, "application/json", "{\"error\":\"Not found\"}");
    }
    
    httpd_send_response(client_socket, response);
    httpd_free_response(response);
    close(client_socket);
}

/* Server thread */
static void httpd_server_thread(void *arg)
{
    (void)arg;
    
    while (httpd_state.running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(httpd_state.server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (httpd_state.running) {
                httpd_log("Error accepting connection: %s\n", strerror(errno));
            }
            continue;
        }
        
        httpd_handle_request(client_socket);
    }
}

/* Public API */
void httpd_init(void)
{
    httpd_state.running = false;
    httpd_state.port = HTTPD_DEFAULT_PORT;
    httpd_state.server_socket = -1;
    httpd_state.server_thread = NULL;
    
    // Register built-in handlers
    httpd_register_handler("/api/status", httpd_api_status);
    httpd_register_handler("/api/floppy/load", httpd_api_floppy_load);
    httpd_register_handler("/api/floppy/eject", httpd_api_floppy_eject);
    
    httpd_log("HTTP server initialized\n");
}

void httpd_start(int port)
{
    if (httpd_state.running) {
        httpd_log("Server already running\n");
        return;
    }
    
    httpd_state.port = port;
    
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        httpd_log("WSAStartup failed\n");
        return;
    }
#endif
    
    httpd_state.server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (httpd_state.server_socket < 0) {
        httpd_log("Error creating socket: %s\n", strerror(errno));
        return;
    }
    
    int opt = 1;
    setsockopt(httpd_state.server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(httpd_state.server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        httpd_log("Error binding socket: %s\n", strerror(errno));
        close(httpd_state.server_socket);
        return;
    }
    
    if (listen(httpd_state.server_socket, HTTPD_MAX_CONNECTIONS) < 0) {
        httpd_log("Error listening on socket: %s\n", strerror(errno));
        close(httpd_state.server_socket);
        return;
    }
    
    httpd_state.running = true;
    httpd_state.server_thread = thread_create(httpd_server_thread, NULL);
    
    httpd_log("HTTP server started on port %d\n", port);
}

void httpd_stop(void)
{
    if (!httpd_state.running) {
        return;
    }
    
    httpd_state.running = false;
    
    if (httpd_state.server_socket >= 0) {
        close(httpd_state.server_socket);
        httpd_state.server_socket = -1;
    }
    
    if (httpd_state.server_thread) {
        thread_wait(httpd_state.server_thread);
        httpd_state.server_thread = NULL;
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    httpd_log("HTTP server stopped\n");
}

void httpd_close(void)
{
    httpd_stop();
    
    // Free routes
    httpd_route_t *route = routes;
    while (route) {
        httpd_route_t *next = route->next;
        free(route->path);
        free(route);
        route = next;
    }
    routes = NULL;
    
    httpd_log("HTTP server closed\n");
}

int httpd_is_running(void)
{
    return httpd_state.running;
}

void httpd_set_port(int port)
{
    httpd_state.port = port;
}

int httpd_get_port(void)
{
    return httpd_state.port;
}
