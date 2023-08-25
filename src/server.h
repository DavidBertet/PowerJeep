#ifndef SERVER_H
#define SERVER_H

#include <esp_http_server.h>

typedef void (*wsserver_receive_callback)(httpd_ws_frame_t* ws_pkt);

void setup_server(void);
esp_err_t broadcast_message(char* msg);

void register_callback(wsserver_receive_callback callback);
void unregister_callback(wsserver_receive_callback callback);

#endif
