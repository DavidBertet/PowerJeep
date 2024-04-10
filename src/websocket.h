#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <esp_http_server.h>

typedef void (*wsserver_receive_callback)(httpd_ws_frame_t* ws_pkt);

void start_websocket(httpd_handle_t server);
void stop_websocket(void);

void on_ws_client_disconnected(int sockfd);

// Send message

esp_err_t broadcast_message(char* msg);

// Listen to message received through callbacks

void register_callback(wsserver_receive_callback callback);
void unregister_callback(wsserver_receive_callback callback);

#endif
