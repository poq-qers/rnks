#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

#include "helpFunctions.h"

void prepareComm(int* udp_socket, struct sockaddr_in6* my_addr, const char* multicast_address, int port) {
    int multicast_ttl = 1; // Link-local scope

    // Create a UDP socket
    if ((*udp_socket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set the multicast TTL
    if (setsockopt(*udp_socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &multicast_ttl, sizeof(multicast_ttl)) < 0) {
        perror("setsockopt error");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    // Configure the multicast address
    memset(my_addr, 0, sizeof(*my_addr));
    my_addr->sin6_family = AF_INET6;
    my_addr->sin6_port = htons(port);
    if (inet_pton(AF_INET6, multicast_address, &my_addr->sin6_addr) <= 0) {
        perror("inet_pton error");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    setnonblocking(udp_socket);
}

void connectPhase(int* udp_socket, struct sockaddr_in6* my_addr, const char* multicast_address, const char* filename, int window_buffersize) {

    printf("Send Message to %s\n", multicast_address);

    // ------------------------------------------- Variablen erstellen -------------------------------------------
    // Time - Var
    struct timeval last_send_time, now, diff;
    struct timeval timeout;
    fd_set read_fds;
    gettimeofday(&last_send_time, NULL);

    struct sockaddr_in6 rec_addr;
    socklen_t address_length = sizeof(rec_addr);

    int first_packet = 1; // erste Paket schicken -> ohne Warten
    char sequence_buffer[window_buffersize][300];
    int escape = 0;

    struct request req;
    struct answer answ;

    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        perror("fopen error");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    // ------------------------------------------- Haupt-Loop -------------------------------------------
    while (1) {
        // ------------------------------------------- Window-Size buffern -------------------------------------------
        for(int i = 0; i < window_buffersize; i++)
            if(fgets(sequence_buffer[i], sizeof(sequence_buffer[i]), file) != NULL)
                if(strcmp(sequence_buffer[i], "escape") == 0) escape = 1;

        // ------------------------------------------- Window-Size senden -------------------------------------------
        for(int i = 0; i < window_buffersize; i++) {

                // ------------------------------------------- Zeitschlitz für NACK -------------------------------------------
                FD_ZERO(&read_fds);
                FD_SET(*udp_socket, &read_fds);

                gettimeofday(&now, NULL);
                timeval_subtract(&diff, &now, &last_send_time);
                long diff_ms = diff.tv_sec * 1000 + diff.tv_usec / 1000;

                timeout.tv_sec = 0;
                if(first_packet)
                    timeout.tv_usec = 0;
                else 
                    timeout.tv_usec = 300000 - diff_ms;

                int select_result = select(*udp_socket + 1, &read_fds, NULL, NULL, &timeout);
                if (select_result == -1) {
                    perror("select error");
                    close(*udp_socket);
                    exit(EXIT_FAILURE);
                }

                // ------------------------------------------- NACK verarbeiten und erneut senden -------------------------------------------
                if (FD_ISSET(*udp_socket, &read_fds)) {
                    int nbytes = recvfrom(*udp_socket, (char*)&answ, sizeof(answ), 0, (struct sockaddr *)&rec_addr, &address_length);
                    if (nbytes < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No data available, try again
                            continue;
                        } else {
                            perror("recvfrom error");
                            close(*udp_socket);
                            exit(EXIT_FAILURE);
                        }
                    }

                    // Nachricht nochmal senden
                    req.ReqType = ReqData;
                    int num = (int)answ.SeNo;
                    req.FlNr = strlen(sequence_buffer[num]); // 1 Char = 1 Byte
                    req.SeNr = num;
                    strcpy(req.name, sequence_buffer[num]);

                    if (sendto(*udp_socket, &req, sizeof(req), 0, (struct sockaddr *)my_addr, sizeof(*my_addr)) < 0) {
                        perror("sendto error");
                        close(*udp_socket);
                        exit(EXIT_FAILURE);
                    }

                    gettimeofday(&last_send_time, NULL);

                 } else {
                 // ------------------------------------------- Nächstes Paket senden -------------------------------------------
                    req.ReqType = ReqData;
                    req.FlNr = strlen(sequence_buffer[i]);
                    req.SeNr = i;
                    strcpy(req.name, sequence_buffer[i]);

                    if (sendto(*udp_socket, &req, sizeof(req), 0, (struct sockaddr *)my_addr, sizeof(*my_addr)) < 0) {
                        perror("sendto error");
                        close(*udp_socket);
                        exit(EXIT_FAILURE);
                    }

                    gettimeofday(&last_send_time, NULL);

                    first_packet = 0;
                 }
        }
        if(escape == 1) break;
    }

    fclose(file);
}

int main(int argc, char* argv[]) {

    int udp_socket = 0;
    struct sockaddr_in6 my_addr;

    // checkInputSend(argc, argv); //-> ausgeschaltet um einfacher zu machen 

    const char* multicast_address = "FF02::1"; //argv[2];
    int port = 9800; //atoi(argv[4]);
    const char* filename = "mytext.txt"; //argv[6];
    int window_buffersize = 2 * 1; // 2*Window ; //atoi(argv[8]);

    prepareComm(&udp_socket, &my_addr, multicast_address, port);
    connectPhase(&udp_socket, &my_addr, multicast_address, filename, window_buffersize);

    // Close the socket
    close(udp_socket);

    return 0;
}