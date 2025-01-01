#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <stdbool.h>

#include "functions.h"
#include "data.h"

#define BUFFER_SIZE 1024

void prepareComm(int* udp_socket, struct sockaddr_in6* my_addr, struct ipv6_mreq* mreq, const char* multicast_address, int port) {
    // Create a UDP socket
    if ((*udp_socket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    // Allow multiple sockets to use the same port number
    int reuse = 1;
    if (setsockopt(*udp_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt error");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    // Bind to the local address and port
    memset(my_addr, 0, sizeof(*my_addr));
    my_addr->sin6_family = AF_INET6;
    my_addr->sin6_port = htons(port);
    my_addr->sin6_addr = in6addr_any;

    if (bind(*udp_socket, (struct sockaddr *)my_addr, sizeof(*my_addr)) < 0) {
        perror("bind");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    // Join the multicast group
    if (inet_pton(AF_INET6, multicast_address, &mreq->ipv6mr_multiaddr) <= 0) {
        perror("inet_pton");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }
    mreq->ipv6mr_interface = 0; // 0 for default interface

    if (setsockopt(*udp_socket, IPPROTO_IPV6, IPV6_JOIN_GROUP, mreq, sizeof(*mreq)) < 0) {
        perror("setsockopt");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    setnonblocking(udp_socket);
}

void sendNack(int* udp_socket, struct sockaddr_in6* rec_addr, struct sockaddr_in6* my_addr, int rightSeq, int serialID) {

    struct answer answ;
    answ.AnswType = AnswNACK;
    answ.SeNo = rightSeq;
    answ.recNr = serialID;

    if (sendto(*udp_socket, (const char*)&answ, sizeof(answ), 0, (struct sockaddr *)rec_addr, sizeof(*rec_addr)) < 0) {
        perror("sendto error");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }
}

bool connectPhase(int* udp_socket, struct sockaddr_in6* my_addr, int serialID, struct sockaddr_in6* rec_addr, socklen_t address_length) {

    struct request req;
    struct answer answ;
    answ.AnswType = AnswHello;
    answ.recNr = serialID;

    struct timeval* timeout = NULL;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(*udp_socket, &read_fds);
    int retval = select(*udp_socket + 1, &read_fds, NULL, NULL, timeout);
    if (retval == -1) {
        perror("select error");
        close(*udp_socket);
        return false;
    } else {
        if (recvfrom(*udp_socket, (char*)&req, sizeof(req), 0, (struct sockaddr *)rec_addr, &address_length) < 0) {
            perror("recvfrom error");
            close(*udp_socket);
            return false;
        }

        if(req.ReqType == ReqHello) {
            if (sendto(*udp_socket, &answ, sizeof(answ), 0, (struct sockaddr *)rec_addr, sizeof(*rec_addr)) < 0) {
                perror("sendto error");
                close(*udp_socket);
                return false;
            }
        }
    }

    return true;
}

void dataPhase(int* udp_socket, struct sockaddr_in6* rec_addr, socklen_t address_length, const char* filename, int serialID, 
                 struct sockaddr_in6* my_addr, int error_packets) {
    
    // ------------------------------------------- Variablen setzen -------------------------------------------
    FILE* file = fopen(filename, "w");
    if (file == NULL) {
        perror("fopen error");
        close(*udp_socket);
        exit(EXIT_FAILURE);
    }

    fd_set read_fds;
    struct timeval timeout;
    int counter = 1;

    // Beispiel: ähnliche Zeitmessung wie beim Sender
    struct timeval slice_start, now, diff;
    gettimeofday(&slice_start, NULL);

    struct request req;

    // Window-Size -> später mit HELLO übermitteln
    int window_size = 1; // alles größer F=1 -> 2*Windowsize oder Modulo -> mal schauen

    int rightSeq = 0; // 0 = immer Start

    // Receive multicast messages
    while (1) { 

        // ----------------------------- Timeout setzen -----------------------------
        FD_ZERO(&read_fds);
        FD_SET(*udp_socket, &read_fds);

        // Set timeout to 300 milliseconds
        timeout.tv_sec = 0;
        timeout.tv_usec = 300000;

        int select_result = select(*udp_socket + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result == -1) {
            perror("select error");
            close(*udp_socket);
            exit(EXIT_FAILURE);
        } else if (select_result == 0) {
            // Timeout occurred, no data available
            printf("No data received in the last 300ms\n");
            continue;
        }

        // ----------------------------- Nachricht empfangen -----------------------------
        if (FD_ISSET(*udp_socket, &read_fds)) {

            int nbytes = recvfrom(*udp_socket, (char*)&req, sizeof(req), 0, (struct sockaddr *)rec_addr, &address_length);
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

            if(req.ReqType == ReqClose) {
                    struct answer answ;
                    answ.AnswType = AnswClose;
                    answ.recNr = serialID;

                    if (sendto(*udp_socket, &answ, sizeof(answ), 0, (struct sockaddr *)rec_addr, sizeof(*rec_addr)) < 0) {
                        perror("sendto error");
                        close(*udp_socket);
                        exit(EXIT_SUCCESS);
                    }
                    break;
            }

            // ----------------------------- restliche Zeit des Zeitschlitzes warten (nie) -----------------------------

            gettimeofday(&now, NULL);
            timeval_subtract(&diff, &now, &slice_start);
            long diff_ms = diff.tv_sec * 1000 + diff.tv_usec / 1000;
            //printf("%ld\n", diff_ms); //-> nie sleepen da immer langsamer
            if(diff_ms < 300) {
                usleep((300 - diff_ms) * 1000);
            }

            // ----------------------------- Nachricht checken -----------------------------
            // Error-Case
            
            /*if(counter == 10 || counter == 20 || counter == 30) {
                req.SeNr = 999L;
            }*/

            // Check
            if(rightSeq != (int)req.SeNr) {
                printf("Send NACK - SN: %d\n", (int)req.SeNr);
                sendNack(udp_socket, rec_addr, my_addr, rightSeq, serialID);
            } else {
                if(rightSeq < window_size) 
                    rightSeq++;
                else 
                   rightSeq = 0;

                // ----------------------------- Nachricht verarbeiten -----------------------------

                printf("Data received - Line %d - SN: %ld\n", counter, req.SeNr);
                fprintf(file, "%s", req.name);
            }
            
            //if (strstr(req.name, "escape") != NULL) break;
            counter++;
        }
    }

    fclose(file);
}

int main(int argc, char* argv[]) {

    if(argc < 2) {
        printf("Gebe bitte alle Argumente mit\n");
        exit(EXIT_FAILURE);
    }

    int udp_socket = 0;
    struct sockaddr_in6 my_addr, rec_addr;
    struct ipv6_mreq mreq;
    socklen_t address_length = sizeof(rec_addr);

    // checkInputRec(argc, argv); //-> kurz um ausgeschaltet um einfacher zu machen 

    const char* multicast_address = "FF02::1"; //argv[2];
    int port = 9800; //atoi(argv[4]);
    const char* filename = "myout.txt"; //argv[6];
    int serialID = atoi(argv[1]); //atoi(argv[8]);

    int error_packets = 10; // atoi(argv[10]);

    prepareComm(&udp_socket, &my_addr, &mreq, multicast_address, port);
    if(!connectPhase(&udp_socket, &my_addr, serialID, &rec_addr, address_length)) {
        printf("Es wurde kein Sender auf dieser Adresse gefunden.\n");
    } else {
        printf("Start Receiving:\n");
        dataPhase(&udp_socket, &rec_addr, address_length, filename, serialID, &my_addr, error_packets);
    }
    

    // Close the socket
    close(udp_socket);

    return 0;
}