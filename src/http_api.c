//
// Created by nacho on 1/9/25.
//

#define HAVE_STDARG_H

#include <http_api.h>
#include <mongoose.h>
#include <pthread.h>
#include <stdarg.h>
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/ini.h>
#include <qt_wrapper.h>

static struct mg_mgr *g_mgr = NULL;
static pthread_t http_thread;
static volatile bool http_running = false;

#ifdef ENABLE_HTTP_API_LOG
int http_api_do_log = ENABLE_HTTP_API_LOG;

static void
http_api_log(const char *fmt, ...)
{
    va_list ap;

    if (http_api_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define http_api_log(fmt, ...)
#endif

static void handle_status(struct mg_connection *c, struct mg_http_message *hm) {
    http_api_log("GET /api/status received\n");

    // Start building the JSON response
    char json_response[1024];
    strcpy(json_response, "{\"status\": \"running\", \"endpoints\": [");

    // Iterate through endpoints and add them to the response
    for (int i = 0; api_endpoints[i].path != NULL; i++) {
        char endpoint_json[256];
        snprintf(endpoint_json, sizeof(endpoint_json),
                "%s\"%s %s\"",
                (i > 0) ? ", " : "",
                api_endpoints[i].method,
                api_endpoints[i].path);
        strcat(json_response, endpoint_json);
    }

    strcat(json_response, "]}");

    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json_response);
}

// Handler functions for each endpoint
static void handle_load_floppy(struct mg_connection *c, struct mg_http_message *hm) {
    http_api_log("POST /api/load_floppy received\n");

    if (hm->body.len > 0) {
        double drive_num = 0;
        if (!mg_json_get_num(hm->body, "$.drive", &drive_num)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                             "{\"status\": \"error\", \"message\": \"Missing drive parameter\"}\n");
            return;
        }

        char* file_path = mg_json_get_str(hm->body, "$.file_path");
        if (!file_path) {
            media_unmount_floppy((int)drive_num);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                     "{\"status\": \"success\", \"message\": \"Floppy unloaded\"}\n");
            return;
        }

        // Load the new floppy image
        media_mount_floppy((int)drive_num, file_path);
        http_api_log("Successfully loaded floppy: %s\n", file_path);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                        "{\"status\": \"success\", \"message\": \"Floppy loaded successfully\"}\n");
    } else {
        http_api_log("Failed to load floppy, missing body.\n");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
             "{\"status\": \"error\", \"message\": \"Failed to load floppy image, missing body.\"}\n");
    }
}

// API endpoints table
const struct api_endpoint api_endpoints[] = {
    {"/api/status",      "GET",  handle_status},
    {"/api/load_floppy", "POST", handle_load_floppy},
    {NULL, NULL, NULL} // Sentinel
};

// Find and execute API handler
static bool handle_api_request(struct mg_connection *c, struct mg_http_message *hm) {
    for (int i = 0; api_endpoints[i].path != NULL; i++) {
        if (mg_strcmp(hm->uri, mg_str(api_endpoints[i].path)) == 0 &&
             mg_strcmp(hm->method, mg_str(api_endpoints[i].method)) == 0) {

            api_endpoints[i].handler(c, hm);
            return true;
             }
    }
    return false;
}

// Send 404 response
static void send_404(struct mg_connection *c) {
    mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                 "{\"status\": \"error\", \"message\": \"Endpoint not found\"}\n");
}

// Main event handler
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;

        // Log all requests
        printf("Request: %.*s %.*s\n",
               (int)hm->method.len, hm->method.buf,
               (int)hm->uri.len, hm->uri.buf);

        // Try to handle API request
        if (handle_api_request(c, hm)) {
            return;
        }

        // Handle root GET request
        if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
          mg_strcmp(hm->uri, mg_str("/")) == 0) {
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n",
                         "Media Server API\nAvailable endpoints:\n"
                         "POST /api/load_floppy\n"
                         "POST /api/load_cdrom\n"
                         "GET  /api/status\n");
            return;
          }

        // Send 404 for unknown endpoints
        send_404(c);
    }
}

static void* http_thread_func(void* arg) {
    struct mg_mgr *mgr = (struct mg_mgr*)arg;

    while (http_running) {
        mg_mgr_poll(mgr, 100);  // Poll every 100ms
    }

    return NULL;
}

int http_api_init(void)
{
    int api_enabled = config_get_int("Api", "api_enabled", 0);
    if (!api_enabled) {
        http_api_log("HTTP API is disabled in configuration.\n");
        return 0;
    }

    http_api_log("Initializing HTTP server...\n");

    // Allocate memory for the manager
    g_mgr = malloc(sizeof(struct mg_mgr));
    if (!g_mgr) {
        http_api_log("Failed to allocate memory for HTTP manager\n");
        return -1;
    }

    // Disable Mongoose logging
    mg_log_set(MG_LL_NONE);

    // Initialize the manager first
    mg_mgr_init(g_mgr);

    int api_port = config_get_int("Api", "api_port", 8080);
    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", api_port);

    // Then set up the listener
    if (!mg_http_listen(g_mgr, listen_url, ev_handler, NULL)) {
        http_api_log("Failed to start HTTP server on %s\n", listen_url);
        mg_mgr_free(g_mgr);
        free(g_mgr);
        g_mgr = NULL;
        return -1;
    }

    http_api_log("Media Server running on %s\n", listen_url);

    http_running = true;
    if (pthread_create(&http_thread, NULL, http_thread_func, g_mgr) != 0) {
        http_api_log("Failed to create HTTP thread\n");
        mg_mgr_free(g_mgr);
        return -1;
    }

    return 0;
}

void http_api_shutdown(void) {
    if (http_running) {
        http_api_log("Shutting down HTTP server...\n");
        http_running = false;
        pthread_join(http_thread, NULL);
        mg_mgr_free(g_mgr);
    }
}
