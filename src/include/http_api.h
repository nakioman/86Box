//
// Created by nacho on 1/9/25.
//

#ifndef INC_86BOX_HTTP_API_H
#define INC_86BOX_HTTP_API_H

#include "mongoose.h"

// API handler function prototypes
typedef void (*api_handler_t)(struct mg_connection *c, struct mg_http_message *hm);

// API endpoint structure
struct api_endpoint {
    const char *path;
    const char *method;
    api_handler_t handler;
};

// Declare the endpoints array as extern
extern const struct api_endpoint api_endpoints[];

int http_api_init(void);

#endif //INC_86BOX_HTTP_API_H
