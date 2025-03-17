/**
 * Copyright (c) 2015, Martin Roth (mhroth@gmail.com)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>

#include <string.h>

#include <errno.h>

#include "globmatch.h"
#include "tinyosc.h"

typedef struct {
    int fd;
    struct sockaddr sa; 
    socklen_t sa_len;
} conT;

typedef struct connectionT {
    conT   send_con;
    conT   receive_con;
    size_t (*send)(struct connectionT *, const void *, size_t);
    size_t (*receive)(struct connectionT *, const void *, size_t);
} connectionT;

typedef struct {
    char *address_match;
    char *format;
    int  (*getter)(tosc_message *, connectionT *);
    int  (*setter)(tosc_message *, connectionT *);
} bioscc_handlerT;

bool bioscc_bidirectional = 1;

void send_error_message(connectionT *conn, char *text) {
    char buffer[128];
    int len;

    len = tosc_writeMessage(buffer, sizeof(buffer), "/error", "s", text);
    conn->send(conn, buffer, len);
}

int ack(tosc_message *message, connectionT *conn) {
    char reply[] = "/ack";
    conn->send(conn, reply, 5);
    return(0);
}

int bidirectional_set(tosc_message *message, connectionT *conn) {
    bioscc_bidirectional = 1;
    ack(message, conn);

    return(0);
}

int test_func(tosc_message *message, connectionT *conn) {
    char buffer[128];
    int len;

    len = tosc_writeMessage(buffer, sizeof(buffer), "/text", "s", "boobies");
    conn->send(conn, buffer, len);

    return(0);
}

int message_to_send_number(tosc_message *m, connectionT *conn) {
    // "/send/X"
    //  0123456
    int d = m->buffer[6] - '1';
    if ((d < 0) || (d > 3)) {
        send_error_message(conn, "invalid send number");
        return (-1);
    }
    return(d);
}

int config_send_source_get(tosc_message *message, connectionT *conn) {
    int send = message_to_send_number(message, conn);
    if (send < 0) return (-1);

    printf ("Send %d -> Source GET\n", send);
    return (0);
}

int config_send_source_set(tosc_message *message, connectionT *conn) {
    int send = message_to_send_number(message, conn);
    if (send < 0) return (-1);
    int source = tosc_getNextInt32(message);

    if ((source < 0) || (source > 3)) {
        send_error_message(conn, "invalid source number");
        return(-1);
    }

    printf ("Send %d -> Source %d\n", send, source);
    return (0);
}

int config_send_brightness_get(tosc_message *message, connectionT *conn) {
    int send = message_to_send_number(message, conn);
    if (send < 0) return (-1);

    printf ("Send %d -> GET Brightness\n", send);
    return (0);
}

int config_send_brightness_set(tosc_message *message, connectionT *conn) {
    int send = message_to_send_number(message, conn);
    if (send < 0) return (-1);

    printf ("Send %d -> GET Brightness\n", send);
    return (0);
}



bioscc_handlerT handlers[] = {
    {"/ack", "", ack, ack},
    {"/bidirectional", "", bidirectional_set, bidirectional_set},
    {"/send/[1-4]/source", "i", config_send_source_get, config_send_source_set}, 
/*
    {"/send/[1-4]/brightness", "f", config_send_brightness_get, config_send_brightness_set},
    {"/send/[1-4]/contrast", "f", config_send_contrast_get, config_send_contrast_set},
    {"/send/[1-4]/saturation", "f", config_send_saturation_get, config_send_saturation_set},
    {"/send/[1-4]/hue", "f", config_send_hue_get, config_send_hue_set},
    {"/send/[1-4]/zoom", "ff", config_send_zoom_get, config_send_zoom_set},
    {"/send/[1-4]/offset", "ff", config_send_offset_get, config_send_offset_set},
    {"/send/[1-4]/rotation", "f",  config_send_rotation_get, config_send_rotation_set},
    {"/config/send_format/resolution", "s", config_send_format_resolution_get, config_send_format_resolution_set},
    {"/config/send_format/framerate", "f", config_send_format_framerate_get, config_send_format_framerate_set},
    {"/config/send_format/colorspace", "s", config_send_format_colorspace_get, config_send_format_colorspace_set},
    {"/config/sync_lock", "i", config_sync_lock_get, config_sync_lock_set},
*/
    {NULL, NULL, NULL, NULL}
};

static volatile bool keepRunning = true;

// handle Ctrl+C
static void sigintHandler(int x) {
  keepRunning = false;
}

void dispatch_message (tosc_message *osc, connectionT *conn) {
    bioscc_handlerT *h;
    int i = 0;

    h = &handlers[i];
    while (h->address_match) {
        // osc->buffer points to the address which is \0 terminated
        if (globmatch(osc->buffer, h->address_match)) {
            if (osc->format[0] == '\0') {
                // no format string, means get
                if (h->getter) h->getter(osc, conn); else send_error_message(conn, "no getter");
            } else {
                if (!strcmp(h->format, osc->format)) {
                    if (h->setter) {
                        h->setter(osc, conn); 
                    } else {
                        send_error_message(conn, "no setter");
                    }
                } else {
                    send_error_message(conn, "format mismatch");
                }
            }
            break;
        }
        h = &handlers[++i];
    }

    if (!h->address_match) {
        send_error_message(conn, "invalid address");
    }
}

size_t send_wrapper(connectionT *conn, const void *buf, size_t len) {
    if (!bioscc_bidirectional) return(0);

    int i;
    char *buffer = (char *) buf;
    printf ("SENDING: ");
    for (i=0; i<len; i++) if (isprint(buffer[i])) printf ("%c", buffer[i]); else  printf ("(%02X)", buffer[i]);
    printf ("\n");

    conn->send_con.sa_len = sizeof(conn->send_con.sa);

    errno = 0;
    i =  sendto(conn->send_con.fd, buf, len, 0, NULL, sizeof(conn->send_con.sa));
    if (errno) {
        perror("sending");
    }

    return (i);
}

int main(int argc, char *argv[]) {
  char buffer[2048];
  connectionT conn;
    
  conn.send = send_wrapper;

  // register the SIGINT handler (Ctrl+C)
  signal(SIGINT, &sigintHandler);

  // open a socket to listen for datagrams (i.e. UDP packets) on port 9000
  conn.receive_con.fd = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(conn.receive_con.fd, F_SETFL, O_NONBLOCK); // set the socket to non-blocking

  struct sockaddr_in sin;

  sin.sin_family = AF_INET;
  sin.sin_port = htons(9000);
  sin.sin_addr.s_addr = INADDR_ANY;
  bind(conn.receive_con.fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_in));

  // open a socket to send
  conn.send_con.fd = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(conn.send_con.fd, F_SETFL, O_NONBLOCK); // set the socket to non-blocking

  sin.sin_port = htons(9010);
  connect(conn.send_con.fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_in));

  printf("tinyosc is now listening on port 9000.\n");
  printf("Press Ctrl+C to stop.\n");

  while (keepRunning) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(conn.receive_con.fd, &readSet);
    struct timeval timeout = {1, 0}; // select times out after 1 second
    if (select(conn.receive_con.fd+1, &readSet, NULL, NULL, &timeout) > 0) {
      int len = 0;
      while ((len = (int) recvfrom(conn.receive_con.fd, buffer, sizeof(buffer), 0, &conn.receive_con.sa, &conn.receive_con.sa_len)) > 0) {
        if (tosc_isBundle(buffer)) {
          tosc_bundle bundle;
          tosc_parseBundle(&bundle, buffer, len);
          const uint64_t timetag = tosc_getTimetag(&bundle);
          printf ("Timetag: %llu\n", timetag);
          tosc_message osc;
          while (tosc_getNextMessage(&bundle, &osc)) {
            dispatch_message(&osc, &conn);
          }
        } else {
          tosc_message osc;
          tosc_parseMessage(&osc, buffer, len);
          dispatch_message(&osc, &conn);
        }
      }
    }
  }

  close(conn.receive_con.fd);
  close(conn.send_con.fd);

  return 0;
}
