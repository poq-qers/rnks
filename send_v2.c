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
int connectPhase(networkContainer* container) {
    // Send HELLO packet
    request hello;
    hello.ReqType = HELLO;
    if (sendto(container->socket, &hello, sizeof(hello), 0,
               (struct sockaddr*)&container->addr, sizeof(container->addr)) < 0) {
        printf("Error sending HELLO packet\n");
        return 0;
    }

    // Wait for HELLO ACK
    answer ack;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT * 1000;
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);

    if (select(container->socket + 1, &read_fds, NULL, NULL, &timeout) > 0) {
        int len = sizeof(container->addr);
        if (recvfrom(container->socket, &ack, sizeof(ack), 0,
                     (struct sockaddr*)&container->addr, &len) < 0) {
            printf("Error receiving HELLO ACK\n");
            return 0;
        }

        if (ack.AnswType == HELLO_ACK) {
            printf("Received HELLO ACK\n");
            return 1;
        }
    }

    printf("No HELLO ACK received\n");
    return 0;
}

// ------------------------------------------------------- close -------------------------------------------------------

int closePhase(networkContainer* container) {
    // Send CLOSE packet
    request close_req;
    close_req.ReqType = CLOSE;
    if (sendto(container->socket, &close_req, sizeof(close_req), 0,
               (struct sockaddr*)&container->addr, sizeof(container->addr)) < 0) {
        printf("Error sending CLOSE packet\n");
        return 0;
    }

    // Wait for CLOSE ACK
    answer ack;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT * 1000;
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(container->socket, &read_fds);

    if (select(container->socket + 1, &read_fds, NULL, NULL, &timeout) > 0) {
        int len = sizeof(container->addr);
        if (recvfrom(container->socket, &ack, sizeof(ack), 0,
                     (struct sockaddr*)&container->addr, &len) < 0) {
            printf("Error receiving CLOSE ACK\n");
            return 0;
        }

        if (ack.AnswType == CLOSE_ACK) {
            printf("Received CLOSE ACK\n");
            return 1;
        }
    }

    printf("No CLOSE ACK received\n");
    return 0;
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

int dataPhase(networkContainer* container) {
    FILE* file = fopen(container->filename, "r");
    if (!file) {
        printf("Error opening file %s\n", container->filename);
        return 0;
    }

    char buffer[1024];
    int seq_num = 0;
    int base = 0;
    struct timeval timeout;
    fd_set write_fds;

    while (fgets(buffer, sizeof(buffer), file)) {
        // Create packet with sequence number
        request packet;
        packet.SeNr = seq_num;
        strcpy(packet.data, buffer);

        // Set timeout interval
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT * 1000;

        // Initialize the file descriptor set
        FD_ZERO(&write_fds);
        FD_SET(container->socket, &write_fds);

        // Wait for the socket to be ready for writing
        if (select(container->socket + 1, NULL, &write_fds, NULL, &timeout) > 0) {
            // Send packet
            if (sendto(container->socket, &packet, sizeof(packet), 0,
                       (struct sockaddr*)&container->addr, sizeof(container->addr)) < 0) {
                printf("Error sending packet\n");
                fclose(file);
                return 0;
            }

            printf("Sent packet with sequence number %d\n", seq_num);
            add_timer(seq_num, TIMEOUT_INTERVAL);
            seq_num++;
        } else {
            printf("Timeout occurred, no data sent\n");
        }

        // Check for NACK
        answer nack;
        FD_ZERO(&write_fds);
        FD_SET(container->socket, &write_fds);
        if (select(container->socket + 1, &write_fds, NULL, NULL, &timeout) > 0) {
            int len = sizeof(container->addr);
            if (recvfrom(container->socket, &nack, sizeof(nack), 0,
                         (struct sockaddr*)&container->addr, &len) > 0) {
                if (nack.AnswType == NACK) {
                    printf("Received NACK for sequence number %d\n", nack.SeNo);
                    // Resend packet
                    fseek(file, nack.SeNo * sizeof(buffer), SEEK_SET);
                    seq_num = nack.SeNo;
                }
            }
        }

        // Decrement timers and move window
        decrement_timer();
        moveWindow(&base);
    }

    // Handle the last packet
    while (base < seq_num) {
        // Check for NACK
        answer nack;
        FD_ZERO(&write_fds);
        FD_SET(container->socket, &write_fds);
        if (select(container->socket + 1, &write_fds, NULL, NULL, &timeout) > 0) {
            int len = sizeof(container->addr);
            if (recvfrom(container->socket, &nack, sizeof(nack), 0,
                         (struct sockaddr*)&container->addr, &len) > 0) {
                if (nack.AnswType == NACK) {
                    printf("Received NACK for sequence number %d\n", nack.SeNo);
                    // Resend packet
                    fseek(file, nack.SeNo * sizeof(buffer), SEEK_SET);
                    seq_num = nack.SeNo;
                }
            }
        }

        // Decrement timers and move window
        decrement_timer();
        moveWindow(&base);
    }

    fclose(file);
    return 1;
}

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