#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdbool.h>

#include "functions.h"

int timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y) {
    if (x->tv_usec < y->tv_usec) {
        int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
        y->tv_usec -= 1000000 * nsec;
        y->tv_sec += nsec;
    }
    if (x->tv_usec - y->tv_usec > 1000000) {
        int nsec = (x->tv_usec - y->tv_usec) / 1000000;
        y->tv_usec += 1000000 * nsec;
        y->tv_sec -= nsec;
    }
    result->tv_sec = x->tv_sec - y->tv_sec;
    result->tv_usec = x->tv_usec - y->tv_usec;
    return x->tv_sec < y->tv_sec;
}

void checkInputSend(int argc, char* argv[]) {
    // Check -> genau 9 Elemente
    if(argc != 9) {
        printf("Aufruf: %s -a <address> -p <port> -f <name> -w <number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Check -> -a, -f, -p ,-w
    if((strcmp(argv[1], "-a") != 0) || strcmp(argv[3], "-p") || (strcmp(argv[5], "-f") != 0) || (strcmp(argv[7], "-w") != 0)) {
        printf("Aufruf: %s -a <address> -p <port> -f <name> -w <number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
}

void checkInputRec(int argc, char* argv[]) {
    if(argc != 11) {
        printf("Usage: %s -a <address> -p <port> -f <output-file> -s <number> -e <number> \n", argv[0]);
        exit(EXIT_FAILURE);
        exit(EXIT_FAILURE);
    }

    // Check -> -a, -f, -w
    if((strcmp(argv[1], "-a") != 0) || strcmp(argv[3], "-p") || (strcmp(argv[5], "-f") != 0) || (strcmp(argv[7], "-s") != 0) || (strcmp(argv[9], "-e"))) {
        printf("Usage: %s -a <address> -p <port> -f <output-file> -s <number> -e <number> \n", argv[0]);
        exit(EXIT_FAILURE);
    }
}

bool setnonblocking(int* sock) {
    int flags, iResult;

    // Get the current flags
    flags = fcntl(*sock, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl failed");
        return false;
    }

    // Set the non-blocking flag
    flags |= O_NONBLOCK;
    iResult = fcntl(*sock, F_SETFL, flags);
    if (iResult == -1) {
        perror("fcntl failed");
        return false;
    }

    return true;
}