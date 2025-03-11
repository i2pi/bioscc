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

bioscc_handlerT handlers[] = {
    {"/info", info_get, NULL},
    {NULL, NULL, NULL}
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
            printf("Matched %s\n", h->address_match);
            tosc_printMessage(osc);
            break;
        }
        h = &handlers[++i];
    }

    if (!h->address_match) {
        printf ("Failed to match\n");
        tosc_printMessage(osc);
    }
}

size_t send_wrapper(connectionT *conn, const void *buf, size_t len) {
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
        errno = 0;
        len = conn.send(&conn, "Reply!", 7);
        if (len < 0) {
            perror ("reply failed :(");
        }
      }
    }
  }

  close(conn.fd);

  return 0;
}
