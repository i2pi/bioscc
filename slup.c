#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>

#include <string.h>
#include <stdlib.h>

#include <errno.h>

#include <libserialport.h>

#include "cobs.h"


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

void print_bytes(const unsigned char *buffer, size_t len) {
    int i;
    for (i=0; i<len; i++) if (isprint(buffer[i])) printf ("%c", buffer[i]); else  printf ("(%02X)", buffer[i]);
}

size_t send_wrapper(connectionT *conn, const void *buf, size_t len) {
    int i;
    printf ("--> UDP: "); print_bytes(buf, len); printf ("\n");

    conn->send_con.sa_len = sizeof(conn->send_con.sa);

    errno = 0;
    i =  sendto(conn->send_con.fd, buf, len, 0, NULL, sizeof(conn->send_con.sa));
    if (errno) {
        perror("sending");
    }

    return (i);
}


/* Helper function for error handling. */
int check(enum sp_return result)
{
        /* For this example we'll just exit on any error by calling abort(). */
        char *error_message;
        switch (result) {
        case SP_ERR_ARG:
                printf("Error: Invalid argument.\n");
                abort();
        case SP_ERR_FAIL:
                error_message = sp_last_error_message();
                printf("Error: Failed: %s\n", error_message);
                sp_free_error_message(error_message);
                abort();
        case SP_ERR_SUPP:
                printf("Error: Not supported.\n");
                abort();
        case SP_ERR_MEM:
                printf("Error: Couldn't allocate memory.\n");
                abort();
        case SP_OK:
        default:
                return result;
        }
}

void list_serial_ports(void)
{
        /* A pointer to a null-terminated array of pointers to
         * struct sp_port, which will contain the ports found.*/
        struct sp_port **port_list;

        /* Call sp_list_ports() to get the ports. The port_list
         * pointer will be updated to refer to the array created. */
        enum sp_return result = sp_list_ports(&port_list);
        if (result != SP_OK) {
                printf("sp_list_ports() failed!\n");
                return;
        }
        /* Iterate through the ports. When port_list[i] is NULL
         * this indicates the end of the list. */
        int i;
        for (i = 0; port_list[i] != NULL; i++) {
                struct sp_port *port = port_list[i];
                /* Get the name of the port. */
                char *port_name = sp_get_port_name(port);
                printf("Found port: %s\n", port_name);
        }
        /* Free the array created by sp_list_ports(). */
        sp_free_port_list(port_list);
        /* Note that this will also free all the sp_port structures
         * it points to. If you want to keep one of them (e.g. to
         * use that port in the rest of your program), take a copy
         * of it first using sp_copy_port(). */
        return;
}

void process_serial_input(struct sp_port *serial_port, connectionT *conn) {
    unsigned char buffer[8192];

    if (sp_input_waiting(serial_port) > 0) {
        unsigned char out_buffer[8192];

        int len = check(sp_blocking_read(serial_port, buffer, 8191, 500));        

        printf ("Ser -->: "); print_bytes(buffer, len); printf ("\n");
        len--; // trailing 0x00

        cobs_decode_result res = cobs_decode(out_buffer, 8191, buffer, len);
        if (res.status != COBS_DECODE_OK) {
            fprintf(stderr, "Failed to cobs decode serial input [%d]\n", res.status);
        }
        send_wrapper(conn, out_buffer, res.out_len);
    }
}

int main(int argc, char *argv[]) {
    unsigned char buffer[2048];
    connectionT conn;
    struct sp_port *serial_port;

    if (argc != 2) {
        fprintf (stderr, "Usage: %s <serial port>\n", argv[0]);
        list_serial_ports();
        exit (-1);
    }

    check(sp_get_port_by_name(argv[1], &serial_port));

    check(sp_open(serial_port, SP_MODE_READ_WRITE));
    printf ("Setting port to 115200 8N1\n");
    check(sp_set_baudrate(serial_port, 115200));
    check(sp_set_bits(serial_port, 8));
    check(sp_set_parity(serial_port, SP_PARITY_NONE));
    check(sp_set_stopbits(serial_port, 1));
    check(sp_set_flowcontrol(serial_port, SP_FLOWCONTROL_NONE));
      
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
      struct timeval timeout = {0, 5000}; 

        process_serial_input(serial_port, &conn);

      if (select(conn.receive_con.fd+1, &readSet, NULL, NULL, &timeout) > 0) {
        int len = 0;
        while ((len = (int) recvfrom(conn.receive_con.fd, buffer, sizeof(buffer), 0, &conn.receive_con.sa, &conn.receive_con.sa_len)) > 0) {
            unsigned char cobs_buffer[8192];
            cobs_encode_result res;

            printf ("UDP -->: "); print_bytes(buffer, len); printf ("\n");

            res = cobs_encode(cobs_buffer, 8191, buffer, len);
            if (res.status == COBS_ENCODE_OK) {
                cobs_buffer[res.out_len++] = '\0';

                printf ("--> Ser: "); print_bytes(cobs_buffer, res.out_len); printf ("\n");
                check(sp_blocking_write(serial_port, cobs_buffer, res.out_len, 1000));
            } else {
                printf ("COBS encode error!\n");
            }
        }
      }
    }

    close(conn.receive_con.fd);
    close(conn.send_con.fd);

    return 0;
}
