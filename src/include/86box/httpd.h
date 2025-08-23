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
#ifndef EMU_HTTPD_H
#define EMU_HTTPD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP server configuration */
#define HTTPD_DEFAULT_PORT 8686
#define HTTPD_MAX_CONNECTIONS 4
#define HTTPD_BUFFER_SIZE 4096

/* HTTP server functions */
extern void httpd_init(void);
extern void httpd_close(void);
extern void httpd_start(int port);
extern void httpd_stop(void);
extern int  httpd_is_running(void);
extern void httpd_set_port(int port);
extern int  httpd_get_port(void);

/* API endpoint handlers */
typedef struct httpd_response {
    int status_code;
    char *content_type;
    char *body;
    size_t body_length;
} httpd_response_t;

typedef httpd_response_t* (*httpd_handler_t)(const char *method, const char *path, const char *query, const char *body);

extern void httpd_register_handler(const char *path, httpd_handler_t handler);
extern httpd_response_t* httpd_create_response(int status_code, const char *content_type, const char *body);
extern void httpd_free_response(httpd_response_t *response);

/* Built-in API handlers */
extern httpd_response_t* httpd_api_floppy_load(const char *method, const char *path, const char *query, const char *body);
extern httpd_response_t* httpd_api_floppy_eject(const char *method, const char *path, const char *query, const char *body);
extern httpd_response_t* httpd_api_status(const char *method, const char *path, const char *query, const char *body);

#ifdef __cplusplus
}
#endif

#endif /*EMU_HTTPD_H*/
