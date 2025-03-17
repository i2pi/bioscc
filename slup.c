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

#include "tinyosc.h"
#include <libserialport.h>

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

static volatile bool keepRunning = true;

// handle Ctrl+C
static void sigintHandler(int x) {
  keepRunning = false;
}

size_t send_wrapper(connectionT *conn, const void *buf, size_t len) {
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
          //  dispatch_message(&osc, &conn);
          }
        } else {
          tosc_message osc;
          tosc_parseMessage(&osc, buffer, len);
          // dispatch_message(&osc, &conn);
          tosc_printMessage(&osc);
        }
      }
    }
  }

  close(conn.receive_con.fd);
  close(conn.send_con.fd);

  return 0;
}
