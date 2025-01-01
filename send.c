#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <stdbool.h>

#include "data.h"
#include "functions.h"

void prepareComm(struct networkContainer* container) {
    int multicast_ttl = 1; // Link-local scope

    // Create a UDP socket
    if ((container->socket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set the multicast TTL
    if (setsockopt(container->socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &multicast_ttl, sizeof(multicast_ttl)) < 0) {
        perror("setsockopt error");
        close(container->socket);
        exit(EXIT_FAILURE);
    }

    memset(&container->my_addr, 0, sizeof(container->my_addr));
    container->my_addr.sin6_family = AF_INET6;
    container->my_addr.sin6_port = htons(container->port);
    if (inet_pton(AF_INET6, container->multicast_address, &container->my_addr.sin6_addr) <= 0) {
        perror("inet_pton error");
        close(container->socket);
        exit(EXIT_FAILURE);
    }

    setnonblocking(&container->socket);
}


// ------------------------------------------------ connect-Phase ------------------------------------------------
bool connectPhase(struct networkContainer* container) {

    struct request req;
    req.ReqType = ReqHello;
    req.FlNr = (long)(container->window_size * 2);

    if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
        perror("sendto error");
        close(container->socket);
        exit(EXIT_FAILURE);
    }

    struct answer answ;
    socklen_t addr_len = sizeof(container->remote_addr);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 3 * 300 * 1000; // 900ms

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);

    int retval = select(container->socket + 1, &read_fds, NULL, NULL, &timeout);
    if (retval == -1) {
        perror("select error");
        close(container->socket);
        return false;
    } else if (retval == 0) {
        close(container->socket);
        return false;
    } else {

        int counter = 0;
        while(true) {
            ssize_t bytes = recvfrom(container->socket, (char*)&answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, &addr_len);
            
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Keine weiteren Daten verfügbar
                    break;
                }
                perror("recvfrom error");
                close(container->socket);
                exit(EXIT_FAILURE);
            }
            if (answ.AnswType == AnswHello) {
                printf("Hello ACK received - recNr: %d\n", answ.recNr);
                container->rec_numbers[counter] = answ.recNr;
                counter++;
            }  

        }
        
    }

    return true;
}

// ------------------------------------------------ close-Phase ------------------------------------------------
bool closePhase(struct networkContainer* container, char sequence_buffer[][300]) {
    
    struct request req;
    req.ReqType = ReqClose;

    if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
        perror("sendto error");
        close(container->socket);
        exit(EXIT_FAILURE);
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 3 * 300 * 1000; // 900ms warten für NACKs oder Close

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);

    struct answer answ;
    socklen_t addr_len = sizeof(container->remote_addr);

    int retval = select(container->socket + 1, &read_fds, NULL, NULL, &timeout);
    if (retval == -1) {
        perror("select error");
        close(container->socket);
        return false;
    } else if (retval == 0) {
        close(container->socket);
        return true;
    } else {

        while(true) {
            ssize_t bytes = recvfrom(container->socket, (char*)&answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, &addr_len);
            
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Keine weiteren Daten verfügbar
                    break;
                }
                perror("recvfrom error");
                close(container->socket);
                exit(EXIT_FAILURE);
            }
            if (answ.AnswType == AnswClose) {
                printf("ACK Close erhalten von rec: %d\n", answ.recNr);
            } else if(answ.AnswType == AnswNACK) {
                int num = answ.SeNo;
                req.ReqType = ReqData;
                req.FlNr = strlen(sequence_buffer[num]);
                req.SeNr = num;
                strcpy(req.name, sequence_buffer[num]);

                if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
                    perror("sendto error");
                    close(container->socket);
                    exit(EXIT_FAILURE);
                }
            }

        }
        
    }

    return true;
}


bool dataPhase(struct networkContainer* container) {

    // ------------------------------------------- Variablen erstellen -------------------------------------------
    // Time - Var
    struct timeval last_send_time, now, diff;
    struct timeval timeout;
    fd_set read_fds;
    gettimeofday(&last_send_time, NULL);

    socklen_t address_length = sizeof(container->remote_addr);

    int first_packet = 1; // erste Paket schicken -> ohne Warten
    int window_buffersize = container->window_size * 2;
    char sequence_buffer[window_buffersize][300];
    int escape = 0;
    bool res = false;

    struct request req;
    struct answer answ;

    FILE* file = fopen(container->filename, "r");
    if (file == NULL) {
        perror("fopen error");
        close(container->socket);
        exit(EXIT_FAILURE);
    }

    // ------------------------------------------- Haupt-Loop -------------------------------------------
    while (1) {
        // ------------------------------------------- Window-Size buffern -------------------------------------------
        for(int i = 0; i < window_buffersize; i++) {
            if(fgets(sequence_buffer[i], sizeof(sequence_buffer[i]), file) != NULL)
            //    if(strcmp(sequence_buffer[i], "escape") == 0) escape = 1;
            if(feof(file)) {
                printf("End of File reached\n");
                window_buffersize = i+1;
                escape = 1;
                break;
            }
        }
        
        // ------------------------------------------- Window-Size senden -------------------------------------------
        for(int i = 0; i < window_buffersize; i++) {

                // ------------------------------------------- Zeitschlitz für NACK -------------------------------------------
                FD_ZERO(&read_fds);
                FD_SET(container->socket, &read_fds);

                gettimeofday(&now, NULL);
                timeval_subtract(&diff, &now, &last_send_time);
                long diff_ms = diff.tv_sec * 1000 + diff.tv_usec / 1000;

                timeout.tv_sec = 0;
                if(first_packet)
                    timeout.tv_usec = 0;
                else 
                    timeout.tv_usec = 300000 - diff_ms;

                int select_result = select(container->socket + 1, &read_fds, NULL, NULL, &timeout);
                if (select_result == -1) {
                    perror("select error");
                    close(container->socket);
                    exit(EXIT_FAILURE);
                }

                // ------------------------------------------- NACK verarbeiten und erneut senden -------------------------------------------
                if (FD_ISSET(container->socket, &read_fds)) {
                    int nbytes = recvfrom(container->socket, (char*)&answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, &address_length);
                    if (nbytes < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No data available, try again
                            continue;
                        } else {
                            perror("recvfrom error");
                            close(container->socket);
                            exit(EXIT_FAILURE);
                        }
                    }

                    /* ### Nachricht nochmal senden -> erstmal nur Mittelung erhalten
                    req.ReqType = ReqData;
                    int num = (int)answ.SeNo;
                    req.FlNr = strlen(sequence_buffer[num]); // 1 Char = 1 Byte
                    req.SeNr = num;
                    strcpy(req.name, sequence_buffer[num]);

                    if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)my_addr, sizeof(*my_addr)) < 0) {
                        perror("sendto error");
                        close(container->socket);
                        exit(EXIT_FAILURE);
                    } */

                    printf("NACK received - SN: %d\n", (int)answ.SeNo);

                    gettimeofday(&last_send_time, NULL);

                 } else {
                 // ------------------------------------------- Nächstes Paket senden -------------------------------------------
                    req.ReqType = ReqData;
                    req.FlNr = strlen(sequence_buffer[i]);
                    req.SeNr = i;
                    strcpy(req.name, sequence_buffer[i]);

                    if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
                        perror("sendto error");
                        close(container->socket);
                        exit(EXIT_FAILURE);
                    }

                    gettimeofday(&last_send_time, NULL);

                    first_packet = 0;
                 }
        }
        if(escape == 1) res = closePhase(container, sequence_buffer);
        if(res) return true;
        //else if(escape == 1) escape++;
    }

    fclose(file);
}

int main(int argc, char* argv[]) {

    struct networkContainer container;
    container.socket = 0;
    container.multicast_address = "FF02::1"; //argv[2];
    container.port = 9800;                   //atoi(argv[4]);
    container.filename = "mytext.txt";       //argv[6];
    container.window_size = 1;               //atoi(argv[8]);

    // checkInputSend(argc, argv); //-> ausgeschaltet um einfacher zu machen 

    prepareComm(&container);
    if(!connectPhase(&container)) {
        printf("No HELLO ACK received.\n");
    } else {
        printf("Start Sending to %s\n", container.multicast_address);
        dataPhase(&container);
    }

    return 0;
}