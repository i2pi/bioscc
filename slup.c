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
#include <unistd.h>

#include "cobs.h"

#define UDP_PORT           9000
#define SERIAL_BAUDRATE    115200
#define SERIAL_TIMEOUT_MS  500    // for blocking read timeout
#define MAX_PACKET_SIZE    2048
#define COBS_BUFFER_SIZE   8192

static volatile bool keep_running = true;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = false;
}

static void print_bytes(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (isprint(buf[i])) putchar(buf[i]);
        else                 printf("(%02X)", buf[i]);
    }
    putchar('\n');
}

static void die_perror(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void exit_on_sp_error(enum sp_return r) {
    if (r == SP_OK)
        return;
    char *err = sp_last_error_message();
    fprintf(stderr, "Serial error: %s\n", err);
    sp_free_error_message(err);
    exit(EXIT_FAILURE);
}

static void list_serial_ports(void) {
    struct sp_port **ports;
    if (sp_list_ports(&ports) != SP_OK) {
        fprintf(stderr, "sp_list_ports() failed\n");
        return;
    }
    for (int i = 0; ports[i]; i++) {
        printf("  %s\n", sp_get_port_name(ports[i]));
    }
    sp_free_port_list(ports);
}

// Read one COBS-framed packet (ending in 0x00) from serial, decode it,
// then send the decoded data back over UDP to the last peer.
static void process_serial_to_udp(struct sp_port *serial,
                                  int udp_fd,
                                  struct sockaddr_storage *peer_addr,
                                  socklen_t peer_len)
{
    uint8_t in_buf[COBS_BUFFER_SIZE];
    ssize_t r = sp_blocking_read(serial, in_buf, sizeof(in_buf), SERIAL_TIMEOUT_MS);
    if (r <= 0) return;  // timeout or error

    // Expect trailing 0x00
    if (in_buf[r-1] != 0) {
        fprintf(stderr, "Warning: serial frame missing terminator\n");
        return;
    }
    size_t framed_len = (size_t)r - 1;

    printf("SER → UDP  : ");
    print_bytes(in_buf, r);

    uint8_t out_buf[COBS_BUFFER_SIZE];
    cobs_decode_result dres = cobs_decode(out_buf, sizeof(out_buf), in_buf, framed_len);
    if (dres.status != COBS_DECODE_OK) {
        fprintf(stderr, "COBS decode failed (%d)\n", dres.status);
        return;
    }

    if (peer_len == 0) {
        fprintf(stderr, "No UDP peer to reply to yet\n");
        return;
    }

    printf("→ UDP (raw): ");
    print_bytes(out_buf, dres.out_len);

    ssize_t sent = sendto(udp_fd, out_buf, dres.out_len, 0,
                          (struct sockaddr*)peer_addr, peer_len);
    if (sent < 0) perror("sendto");
}

// Read one UDP datagram, COBS-encode it, and write to serial.
static void process_udp_to_serial(int udp_fd, struct sp_port *serial,
                                  struct sockaddr_storage *peer_addr,
                                  socklen_t *peer_len)
{
    uint8_t buf[MAX_PACKET_SIZE];
    struct sockaddr_storage src;
    socklen_t src_len = sizeof(src);

    ssize_t len = recvfrom(udp_fd, buf, sizeof(buf), 0,
                           (struct sockaddr*)&src, &src_len);
    if (len <= 0) return;

    // Remember who sent it for replies
    memcpy(peer_addr, &src, src_len);
    *peer_len = src_len;

    printf("UDP → SER  : ");
    print_bytes(buf, len);

    uint8_t cobs_buf[COBS_BUFFER_SIZE];
    cobs_encode_result eres = cobs_encode(cobs_buf, sizeof(cobs_buf)-1, buf, (size_t)len);
    if (eres.status != COBS_ENCODE_OK) {
        fprintf(stderr, "COBS encode failed (%d)\n", eres.status);
        return;
    }
    // append terminator
    cobs_buf[eres.out_len++] = 0x00;

    printf("→ SER (raw): ");
    print_bytes(cobs_buf, eres.out_len);

    ssize_t written = sp_blocking_write(serial, cobs_buf, eres.out_len, 1000);
    if (written < 0) {
        char *msg = sp_last_error_message();
        fprintf(stderr, "serial write error: %s\n", msg);
        sp_free_error_message(msg);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <serial-port-name>\n", argv[0]);
        printf("Available ports:\n");
        list_serial_ports();
        return EXIT_FAILURE;
    }

    // set up Ctrl-C handler
    signal(SIGINT, handle_sigint);

    // open and configure serial port
    struct sp_port *serial;
    exit_on_sp_error(sp_get_port_by_name(argv[1], &serial));
    exit_on_sp_error(sp_open(serial, SP_MODE_READ_WRITE));
    printf("Opened %s at %d-8-N-1\n", argv[1], SERIAL_BAUDRATE);
    exit_on_sp_error(sp_set_baudrate(serial, SERIAL_BAUDRATE));
    exit_on_sp_error(sp_set_bits(serial, 8));
    exit_on_sp_error(sp_set_parity(serial, SP_PARITY_NONE));
    exit_on_sp_error(sp_set_stopbits(serial, 1));
    exit_on_sp_error(sp_set_flowcontrol(serial, SP_FLOWCONTROL_NONE));

    // open non-blocking UDP socket
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) die_perror("socket");
    fcntl(udp_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = htons(UDP_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(udp_fd, (struct sockaddr*)&local, sizeof(local)) < 0)
        die_perror("bind");

    printf("Listening on UDP port %d\n", UDP_PORT);

    // track last UDP peer for replies
    struct sockaddr_storage peer_addr = {0};
    socklen_t peer_len = 0;

    // main loop
    while (keep_running) {
        // first handle any incoming serial → UDP
        process_serial_to_udp(serial, udp_fd, &peer_addr, peer_len);

        // then wait briefly for UDP → serial
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_fd, &rfds);
        struct timeval tv = { 0, 5000 };  // 5 ms
        if (select(udp_fd+1, &rfds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(udp_fd, &rfds))
                process_udp_to_serial(udp_fd, serial, &peer_addr, &peer_len);
        }
    }

    // clean up
    close(udp_fd);
    sp_close(serial);
    sp_free_port(serial);
    printf("Terminated cleanly\n");
    return 0;
}

