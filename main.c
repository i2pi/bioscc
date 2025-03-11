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

#include <errno.h>

#include "globmatch.h"
#include "tinyosc.h"

typedef struct connectionT {
    int   fd;
    struct sockaddr sa; 
    socklen_t sa_len;
    size_t (*send)(struct connectionT *, const void *, size_t);
    size_t (*receive)(struct connectionT *, const void *, size_t);
} connectionT;

typedef struct {
    char *address_match;
    int  (*getter)(tosc_message *, connectionT *);
    int  (*setter)(tosc_message *, connectionT *);
} bioscc_handlerT;

bool bioscc_bidirectional = 0;

int info_get (tosc_message *m, connectionT *conn) {
  char buffer[2048];
  int len = 0;

  len = tosc_writeMessage(buffer, sizeof(buffer), "/info", "fsi", 1.0f, "hello world", -1);
  errno = 0;
  conn->send(conn, buffer, len);
  if (errno) {
    perror("info get");
  }

  return(0);
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

bioscc_handlerT handlers[] = {
    {"/ack", ack, ack},
    {"/bidirectional", bidirectional_set, bidirectional_set},
/*
    {"/status", status_get, NULL},
    {"/config/sends/[1-4]/source", config_send_source_get, config_send_source_set},
    {"/config/sends/[1-4]/brightness", config_send_brightness_get, config_send_brightness_set},
    {"/config/sends/[1-4]/contrast", config_send_contrast_get, config_send_contrast_set},
    {"/config/sends/[1-4]/saturation", config_send_saturation_get, config_send_saturation_set},
    {"/config/sends/[1-4]/hue", config_send_hue_get, config_send_hue_set},
    {"/config/sends/[1-4]/zoom", config_send_zoom_get, config_send_zoom_set},
    {"/config/send_format/resolution", config_send_format_resolution_get, config_send_format_resolution_set},
    {"/config/send_format/framerate", config_send_format_framerate_get, config_send_format_framerate_set},
    {"/config/send_format/colorspace", config_send_format_colorspace_get, config_send_format_colorspace_set},
    {"/config/sync_lock", config_sync_lock_get, config_sync_lock_set},
*/
    {NULL, NULL, NULL}
};

static volatile bool keepRunning = true;

// handle Ctrl+C
static void sigintHandler(int x) {
  keepRunning = false;
}

void send_error_message(connectionT *conn, char *text) {
    char buffer[128];
    int len;

    len = tosc_writeMessage(buffer, sizeof(buffer), "/error", "s", text);
    conn->send(conn, buffer, len);
}

void dispatch_message (tosc_message *osc, connectionT *conn) {
    bioscc_handlerT *h;
    int i = 0;

    h = &handlers[i];
    while (h->address_match) {
        // osc->buffer points to the address which is \0 terminated
        if (globmatch(osc->buffer, h->address_match)) {
            printf("Matched %s\n", h->address_match);
            if (osc->format[0] == '\0') {
                // no format string, means get
                printf ("No format string - get\n");
                if (h->getter) h->getter(osc, conn); else send_error_message(conn, "no getter");
            } else {
                printf ("Has format string - set\n");
                if (h->setter) h->setter(osc, conn); else send_error_message(conn, "no setter");
            }
            tosc_printMessage(osc);
            break;
        }
        h = &handlers[++i];
    }

    if (!h->address_match) {
        send_error_message(conn, "invalid address");
        printf ("Failed to match\n");
        tosc_printMessage(osc);
    }
}

size_t send_wrapper(connectionT *conn, const void *buf, size_t len) {
    if (!bioscc_bidirectional) return(0);

    int i;
    char *buffer = (char *) buf;
    printf ("SENDING: ");
    for (i=0; i<len; i++) if (isprint(buffer[i])) printf ("%c", buffer[i]); else  printf ("(%02X)", buffer[i]);
    printf ("\n");

    conn->sa_len = sizeof(conn->sa);
    return(sendto(conn->fd, buf, len, 0, &conn->sa, conn->sa_len));
}

int main(int argc, char *argv[]) {
  char buffer[2048];
  connectionT conn;
    
  conn.send = send_wrapper;

  // register the SIGINT handler (Ctrl+C)
  signal(SIGINT, &sigintHandler);

  // open a socket to listen for datagrams (i.e. UDP packets) on port 9000
  conn.fd = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(conn.fd, F_SETFL, O_NONBLOCK); // set the socket to non-blocking

  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_port = htons(9000);
  sin.sin_addr.s_addr = INADDR_ANY;
  bind(conn.fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_in));

  printf("tinyosc is now listening on port 9000.\n");
  printf("Press Ctrl+C to stop.\n");

  while (keepRunning) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(conn.fd, &readSet);
    struct timeval timeout = {1, 0}; // select times out after 1 second
    if (select(conn.fd+1, &readSet, NULL, NULL, &timeout) > 0) {
      int len = 0;
      while ((len = (int) recvfrom(conn.fd, buffer, sizeof(buffer), 0, &conn.sa, &conn.sa_len)) > 0) {
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

  close(conn.fd);

  return 0;
}
