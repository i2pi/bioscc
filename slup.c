#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libserialport.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "cobs.h"

#define UDP_PORT           9000
#define SERIAL_BAUDRATE    115200
#define MAX_PACKET_SIZE    2048
#define COBS_BUFFER_SIZE   8192

static volatile bool keep_running = true;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = false;
}

static void die_perror(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void exit_on_sp_error(enum sp_return r) {
    if (r != SP_OK) {
        char *err = sp_last_error_message();
        fprintf(stderr, "Serial error: %s\n", err);
        sp_free_error_message(err);
        exit(EXIT_FAILURE);
    }
}

static void print_bytes(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (isprint(buf[i])) putchar(buf[i]);
        else                 printf("(%02X)", buf[i]);
    }
    putchar('\n');
}

static double diff_ms(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0
         + (b->tv_nsec - a->tv_nsec) / 1e6;
}

static void process_serial_to_udp(struct sp_port *serial,
                                  int udp_fd,
                                  struct sockaddr_storage *peer_addr,
                                  socklen_t peer_len)
{
    uint8_t in_buf[COBS_BUFFER_SIZE];
    ssize_t r;
    while ((r = sp_nonblocking_read(serial, in_buf, sizeof(in_buf))) > 0) {
        if (in_buf[r - 1] != 0) {
            fprintf(stderr, "Warning: serial frame missing terminator\n");
            continue;
        }
        size_t framed_len = (size_t)r - 1;

        printf("SER → UDP  : ");
        print_bytes(in_buf, r);

        uint8_t out_buf[COBS_BUFFER_SIZE];
        cobs_decode_result dres =
            cobs_decode(out_buf, sizeof(out_buf), in_buf, framed_len);
        if (dres.status != COBS_DECODE_OK) {
            fprintf(stderr, "COBS decode error %d\n", dres.status);
            continue;
        }

        if (peer_len == 0) {
            fprintf(stderr, "No UDP peer set\n");
            continue;
        }

        printf("→ UDP (raw): ");
        print_bytes(out_buf, dres.out_len);

        if (sendto(udp_fd, out_buf, dres.out_len, 0,
                   (struct sockaddr*)peer_addr, peer_len) < 0)
        {
            perror("sendto");
        }
    }
}

static void process_udp_to_serial(int udp_fd,
                                  struct sp_port *serial,
                                  struct sockaddr_storage *peer_addr,
                                  socklen_t *peer_len)
{
    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint8_t buf[MAX_PACKET_SIZE];
    struct sockaddr_storage src;
    socklen_t src_len = sizeof(src);

    ssize_t len = recvfrom(udp_fd, buf, sizeof(buf), 0,
                           (struct sockaddr*)&src, &src_len);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (len <= 0) return;

    memcpy(peer_addr, &src, src_len);
    *peer_len = src_len;

    printf("UDP → SER  : ");
    print_bytes(buf, len);

    uint8_t cobs_buf[COBS_BUFFER_SIZE];
    cobs_encode_result eres =
        cobs_encode(cobs_buf, sizeof(cobs_buf) - 1, buf, (size_t)len);
    if (eres.status != COBS_ENCODE_OK) {
        fprintf(stderr, "COBS encode error %d\n", eres.status);
        return;
    }
    cobs_buf[eres.out_len++] = 0x00;
    clock_gettime(CLOCK_MONOTONIC, &t2);

    printf("→ SER (raw): ");
    print_bytes(cobs_buf, eres.out_len);

    ssize_t written = sp_blocking_write(serial, cobs_buf, eres.out_len, 100);
    clock_gettime(CLOCK_MONOTONIC, &t3);
    if (written < 0) {
        char *msg = sp_last_error_message();
        fprintf(stderr, "serial write error: %s\n", msg);
        sp_free_error_message(msg);
    }

    fprintf(stderr,
            "timing: recv=%.2fms encode=%.2fms write=%.2fms bytes=%zu\n",
            diff_ms(&t0, &t1),
            diff_ms(&t1, &t2),
            diff_ms(&t2, &t3),
            (size_t)eres.out_len);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <serial-port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // install SIGINT handler (no SA_RESTART so select() is interruptible)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        die_perror("sigaction");
    }

    // open and configure serial port
    struct sp_port *serial;
    exit_on_sp_error(sp_get_port_by_name(argv[1], &serial));
    exit_on_sp_error(sp_open(serial, SP_MODE_READ_WRITE));
    exit_on_sp_error(sp_set_baudrate(serial, SERIAL_BAUDRATE));
    exit_on_sp_error(sp_set_bits(serial, 8));
    exit_on_sp_error(sp_set_parity(serial, SP_PARITY_NONE));
    exit_on_sp_error(sp_set_stopbits(serial, 1));
    exit_on_sp_error(sp_set_flowcontrol(serial, SP_FLOWCONTROL_NONE));
    printf("Opened %s at %d-8-N-1\n", argv[1], SERIAL_BAUDRATE);

    // extract the underlying file descriptor
    void *handle;
    exit_on_sp_error(sp_get_port_handle(serial, &handle));
    int serial_fd = (int)(intptr_t)handle;

    // set up UDP socket
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) die_perror("socket");
    fcntl(udp_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_port        = htons(UDP_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(udp_fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        die_perror("bind");
    }
    printf("Listening on UDP port %d\n", UDP_PORT);

    struct sockaddr_storage peer_addr = {0};
    socklen_t peer_len = 0;

    while (keep_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_fd,    &rfds);
        FD_SET(serial_fd, &rfds);

        int maxfd = udp_fd > serial_fd ? udp_fd : serial_fd;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) break;
            perror("select");
            break;
        }

        if (FD_ISSET(serial_fd, &rfds)) {
            process_serial_to_udp(serial, udp_fd, &peer_addr, peer_len);
        }
        if (FD_ISSET(udp_fd, &rfds)) {
            process_udp_to_serial(udp_fd, serial, &peer_addr, &peer_len);
        }
    }

    close(udp_fd);
    sp_close(serial);
    sp_free_port(serial);
    printf("Terminated cleanly\n");
    return 0;
}

