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

#include "timer.h"
#include "functions.h"
#include "data.h"

#define BUFFER_SIZE 1024

// ------------------------------------------------------- prepare ------------------------------------------------------
bool prepareComm(networkContainer* container) {
    struct ipv6_mreq mreq;

    // Create a UDP socket
    if ((container->socket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        return false;
    }

    // Allow multiple sockets to use the same port number
    int reuse = 1;
    if (setsockopt(container->socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt error");
        return false;
    }

    // Bind to the local address and port
    memset(&container->my_addr, 0, sizeof(container->my_addr));
    container->my_addr.sin6_family = AF_INET6;
    container->my_addr.sin6_port = htons(container->port);
    container->my_addr.sin6_addr = in6addr_any;

    if (bind(container->socket, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
        perror("bind");
        return false;
    }

    // Join the multicast group
    if (inet_pton(AF_INET6, container->multicast_address, &mreq.ipv6mr_multiaddr) <= 0) {
        perror("inet_pton");
        return false;
    }
    mreq.ipv6mr_interface = 0; // 0 for default interface

    if (setsockopt(container->socket, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt");
        return false;
    }

    if(!setnonblocking(&container->socket)) return false;

    return true;
}

// ------------------------------------------------------- send NACK ------------------------------------------------------
void sendNack(networkContainer* container, int rightSeq) {

    answer answ;
    answ.AnswType = AnswNACK;
    answ.SeNo = rightSeq;
    answ.recNr = container->serialID;

    if (sendto(container->socket, (const char*)&answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, sizeof(container->remote_addr)) < 0) {
        perror("sendto error");
        exit(EXIT_FAILURE);
    }
}

// ------------------------------------------------------- connect ------------------------------------------------------
bool connectPhase(networkContainer* container , socklen_t* address_length) {

    request req;
    answer answ;
    answ.AnswType = AnswHello;
    answ.recNr = container->serialID;

    struct timeval* timeout = NULL;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);
    int retval = select(container->socket + 1, &read_fds, NULL, NULL, timeout);
    if (retval == -1) {
        perror("select error");
        return false;
    } else {
        if (recvfrom(container->socket, (char*)&req, sizeof(req), 0, (struct sockaddr *)&container->remote_addr, address_length) < 0) {
            perror("recvfrom error");
            return false;
        }

        container->window_size = req.FlNr;

        if(req.ReqType == ReqHello) {
            if (sendto(container->socket, &answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, sizeof(container->remote_addr)) < 0) {
                perror("sendto error");
                return false;
            }
        }
    }

    return true;
}
// ------------------------------------------------------- close ------------------------------------------------------
bool sendClose(networkContainer* container) {
    answer answ;
    answ.AnswType = AnswClose;
    answ.recNr = container->serialID;

    if (sendto(container->socket, &answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, sizeof(container->remote_addr)) < 0) {
        perror("sendto error");
        return false;
    }
    return true;
}

// ------------------------------------------------------- data ------------------------------------------------------
bool receiveFirstPacket(networkContainer* container, socklen_t* address_length, request* buffer, int* rightReq, FILE* file) {
    request req;
    fd_set read_fds;
    struct timeval* timeout = NULL;

    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);

    int retval = select(container->socket + 1, &read_fds, NULL, NULL, timeout);
    if (retval == -1) {
        perror("select error");
        return false;
    } else {
        if (recvfrom(container->socket, (char*)&req, sizeof(req), 0, (struct sockaddr *)&container->remote_addr, address_length) < 0) {
            perror("recvfrom error");
            return false;
        }

        printf("Received SN: %d\n", *rightReq);
        buffer[*rightReq] = req;
        (*rightReq)++;
        fprintf(file, "%s", req.name);

        return true;

    }

    return false;
}

bool dataPhase(networkContainer* container, socklen_t* address_length) {
    
    request buffer[2 * MAX_WINDOW_SIZE];

    int base = 0;
    int window_size = 0;
    bool nack = false;
    request nackBuffer[MAX_WINDOW_SIZE];
    int nackCounter = 0;

    for(int i = 0; i < 10; i++)
        nackBuffer[i].SeNr = 999L;

    // später fixen
    if(container->window_size == 1) {
        window_size = 2 * container->window_size;
    } else {
        window_size = container->window_size;
    }

    // ------------------------------------------- Variablen setzen -------------------------------------------
    FILE* file = fopen(container->filename, "w");
    if (file == NULL) {
        perror("fopen error");
        return false;
    }

    fd_set read_fds;
    struct timeval timeout;

    // Beispiel: ähnliche Zeitmessung wie beim Sender
    struct timeval slice_start; // now, diff;
    gettimeofday(&slice_start, NULL);

    request req;
    int rightSeq = 0; // 0 = immer Start
    //bool firstTry = true; // -> nicht mehr auskommentieren, wenn NACK-testcase eingebaut

    if(!receiveFirstPacket(container, address_length, buffer, &rightSeq, file)) return false;
    else base++;
    // Einfach ausgehen, dass das erste Paket richtig ist

    // Receive multicast messages
    while (true) { 

        FD_ZERO(&read_fds);
        FD_SET(container->socket, &read_fds);

        // Set timeout to 300 milliseconds
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_INTERVAL * TIMEOUT * 1000;

        int select_result = select(container->socket + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result == -1) {
            perror("select error");
            return false;
        } else if (select_result == 0) {
            printf("No data received in the last 900ms\n");
            // SendNack(base) -> implementieren
        }

        // receive packet
        if (FD_ISSET(container->socket, &read_fds)) {
            int nbytes = recvfrom(container->socket, (char*)&req, sizeof(req), 0, (struct sockaddr *)&container->remote_addr, address_length);
            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No data available, try again
                    continue;
                } else {
                    perror("recvfrom error");
                    return false;
                }
            }

            int SN = (int)req.SeNr;

            /* NACK testcase
            if(SN == 10 && firstTry == true) {
                SN = 11;
                firstTry = false;
            }*/

            if(req.ReqType == ReqClose) {
                sendClose(container);
                break;
            }

            if(base == (int)req.SeNr) nack = false;
            // buffern, nochmal schicken, gucken ob die anderen richtig sind
            if(!nack) {
                if (base <= SN && SN < (base + window_size)) {
                    buffer[base % window_size] = req;
                    if(SN != base) {
                        nackCounter = 0;
                        printf("Send NACK - SN: %d, - base: %d\n", SN, base);
                        nack = true;
                        sendNack(container, base);
                    } else {
                            if(req.ReqType == ReqClose) {
                                sendClose(container);
                                break;
                            } else if(req.ReqType == ReqData) {

                                printf("Received SN: %d\n", SN);
                                fprintf(file, "%s", req.name);

                                for(int i = 0; i < 10; i++) {
                                    if((int)nackBuffer[i].SeNr != 999) {
                                        printf("Received SN: %d\n", (int)nackBuffer[i].SeNr);
                                        fprintf(file, "%s", nackBuffer[i].name);
                                        base++;
                                        nackBuffer[i].SeNr = 999L;
                                    }
                                }
                    
                                base++;
                            }

                    } 
                } else {
                    printf("SN: %d - nicht im window\n", SN);
                }
            } else {
                nackBuffer[nackCounter] = req;
                nackCounter++; 
            }
            
        }
        
    }

    fclose(file);
    return true;
}

int main(int argc, char* argv[]) {

    if(argc < 2) {
        printf("Gebe bitte alle Argumente mit\n");
        return 1;
    }

    // checkInputRec(argc, argv); //-> kurz um ausgeschaltet um einfacher zu machen 

    networkContainer container;
    container.socket = 0;
    container.multicast_address = "FF02::01"; //argv[2];
    container.port = 9800;                    //atoi(argv[4]);
    container.filename = "myout.txt";         //argv[6];
    container.serialID = atoi(argv[1]);       //atoi(argv[8]);
    container.error_packets = 10;             // atoi(argv[10]); -> jedes x. Paket geht verloren

    socklen_t address_length = sizeof(container.remote_addr);

    if(prepareComm(&container)) {
        if(connectPhase(&container, &address_length)) {
            printf("Start Receiving:\n");
            if(!dataPhase(&container, &address_length))
                printf("Error while receiving.\n");
        } else {
            printf("Reached no one.\n");
        }
    }

    close(container.socket);
    return 0;
}