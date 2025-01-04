#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <stdbool.h>

#include "timer.h"
#include "data.h"
#include "functions.h"

// ------------------------------------------------------- prepare -------------------------------------------------------
bool prepareComm(networkContainer* container) {
    int multicast_ttl = 1; // Link-local scope

    // Create a UDP socket
    if ((container->socket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        return false;
    }

    // Set the multicast TTL
    if (setsockopt(container->socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &multicast_ttl, sizeof(multicast_ttl)) < 0) {
        perror("setsockopt error");
        return false;
    }

    memset(&container->my_addr, 0, sizeof(container->my_addr));
    container->my_addr.sin6_family = AF_INET6;
    container->my_addr.sin6_port = htons(container->port);
    if (inet_pton(AF_INET6, container->multicast_address, &container->my_addr.sin6_addr) <= 0) {
        perror("inet_pton error");
        return false;
    }

    if(!setnonblocking(&container->socket)) return false;

    return true;
}


// ------------------------------------------------------- connect -------------------------------------------------------
bool connectPhase(networkContainer* container) {

    request req;
    req.ReqType = ReqHello;
    req.FlNr = (long)(container->window_size);

    if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
        perror("sendto error");
        return false;
    }

    answer answ;
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
        return false;
    } else if (retval == 0) {
        return false;
    } else {

        int counter = 0;
        while(true) {
            ssize_t bytes = recvfrom(container->socket, (char*)&answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, &addr_len);
            
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Keine weiteren Daten verfügbar

                perror("recvfrom error");
                return false;
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

// ------------------------------------------------------- close ------------------------------------------------------
bool closePhase(networkContainer* container, int nextSN) {
    char sequence_buffer[50][300];
    
    request req;
    req.ReqType = ReqClose;
    req.SeNr = (long)nextSN;

    if (sendto(container->socket, &req, sizeof(req), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
        perror("sendto error");
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 3 * 300 * 1000; // 900ms warten für NACKs oder Close

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);

    answer answ;
    socklen_t addr_len = sizeof(container->remote_addr);

    int retval = select(container->socket + 1, &read_fds, NULL, NULL, &timeout);
    if (retval == -1) {
        perror("select error");
        return false;
    } else if (retval == 0) {
        return true;
    } else {

        while(true) {
            ssize_t bytes = recvfrom(container->socket, (char*)&answ, sizeof(answ), 0, (struct sockaddr *)&container->remote_addr, &addr_len);
            
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Keine weiteren Daten verfügbar

                perror("recvfrom error");
                return false;
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
                    return false;
                }
            }

        }
        
    }

    return true;
}

// ------------------------------------------------------- data ------------------------------------------------------
void showWindow(request window[], int base) {
    // Show Window-Size
        for(int i = 0; i < 20; i++) {
            request* pkt = &window[i];
            if(pkt->SeNr != 999L) {
                printf("%ld ", pkt->SeNr);
            } else {
                printf("X ");
            }
        }
        printf("  %d\n", base);
}

bool dataPhase(networkContainer* container) {
    
    answer answ;
    int window_size = container->window_size;
    
    FILE* file = fopen(container->filename, "r");
    if (file == NULL) {
        perror("fopen error");
        return false;
    }

    int base = 0;
    int nextSN = 0;
    request window[2 * MAX_WINDOW_SIZE];
    char line[300];

    struct timeval timeout;
    fd_set read_fds;
    struct timeval last_send_time, now, diff;
    gettimeofday(&last_send_time, NULL);

    for(int i = 0; i < 20; i++)
        window[i].SeNr = 999L;

    int noNackCount = 0; // Add this counter

    // ------------------------------------------- Haupt-Loop -------------------------------------------
    while (true) {

        while(nextSN < base + window_size && fgets(line, sizeof(line), file)) {

            request* packet = &window[nextSN % window_size];
            packet->ReqType = ReqData;
            packet->SeNr = nextSN;
            strncpy(packet->name, line, PufferSize - 1);

            if (sendto(container->socket, packet, sizeof(*packet), 0, (struct sockaddr *)&container->my_addr, sizeof(container->my_addr)) < 0) {
                    perror("sendto error");
                    return false;
            }
            add_timer(nextSN, TIMEOUT_INTERVAL);
            nextSN++;
        }
        gettimeofday(&last_send_time, NULL);

        FD_ZERO(&read_fds);
        FD_SET(container->socket, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 300000; // 300 ms

        int res_sel = select(container->socket + 1, &read_fds, NULL, NULL, &timeout);

        if(res_sel == -1) {
            perror("select");
        } else if (res_sel == 0) {
            noNackCount++; // Increment counter when no NACK received
            
            // After 3 timeouts, move window for F=1
           if (noNackCount >= 3) {
                decrement_timer();
                //del_timer(base);
                base = base + window_size; // Move window by one position (F=1)
                noNackCount = 0; // Reset counter
                //printf("Timeout after 3 intervals - moving window to base: %d\n", base);
            }
        } else if(FD_ISSET(container->socket, &read_fds)) {
            socklen_t addr_len = sizeof(container->remote_addr);
            if (recvfrom(container->socket, &answ, sizeof(answ), 0, (struct sockaddr*)&container->remote_addr, &addr_len) < 0) {
                perror("Error receiving NACK");
            } else {
                // Prüfung auf richtigen Empfänger
                bool inList = false;
                for(int i = 0; i < 10; i++) {
                    if(container->rec_numbers[i] == answ.recNr) inList = true;
                }

                if(inList) {
                    noNackCount = 0;
                    printf("NACK erhalten - SN: %d\n", answ.SeNo);

                    del_timer(answ.SeNo);

                    request* packet = &window[answ.SeNo % window_size];
                    if (sendto(container->socket, packet, sizeof(*packet), 0, (struct sockaddr *)&container->remote_addr, sizeof(container->remote_addr)) < 0) {
                        perror("sendto error");
                        return false;
                    }

                    add_timer(answ.SeNo, TIMEOUT_INTERVAL);
                } else {
                    printf("NACK received, but %d not in list\n", answ.SeNo);
                }

            }
        }

        //showWindow(window, base);

        gettimeofday(&now, NULL);
        timeval_subtract(&diff, &now, &last_send_time);
        long diff_ms = diff.tv_sec * 1000 + diff.tv_usec / 1000;
        if (diff_ms < 300) {
            usleep((300 - diff_ms) * 1000);
        }

        moveWindow(&base);

        if(feof(file) && base == nextSN) {
            printf("EOF reached.\n");
            if(closePhase(container, nextSN)) {
                printf("Programm erfolreich geschlossen.\n");
                break;
            }
        }
    }

    fclose(file);
    return true;
}

int main(int argc, char* argv[]) {

    // checkInputSend(argc, argv); //-> ausgeschaltet um einfacher zu machen 

    networkContainer container;
    container.socket = 0;
    container.multicast_address = "FF02::1"; //argv[2];
    container.port = 9800;                   //atoi(argv[4]);
    container.filename = "mytext.txt";       //argv[6];
    container.window_size = 1;               //atoi(argv[8]);

    for(int i = 0; i < 10; i++)
        container.rec_numbers[i] = -1;

    if(prepareComm(&container)) {
        if(connectPhase(&container)) {
            printf("Start Sending to %s\n", container.multicast_address);
            if(!dataPhase(&container))
                printf("Error while sending data.\n");
        } else {
            printf("No HELLO ACK received.\n");
        }
    }

    close(container.socket);
    return 0;
}