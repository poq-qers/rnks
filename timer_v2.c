#include "timer.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct Timer {
    int seq_num;
    int ticks;
    struct Timer* next;
} Timer;

static Timer* timer_list = NULL;

void add_timer(int seq_num, int ticks) {
    Timer* new_timer = (Timer*)malloc(sizeof(Timer));
    new_timer->seq_num = seq_num;
    new_timer->ticks = ticks;
    new_timer->next = timer_list;
    timer_list = new_timer;
}

void del_timer(int seq_num) {
    Timer** current = &timer_list;
    while (*current) {
        Timer* entry = *current;
        if (entry->seq_num == seq_num) {
            *current = entry->next;
            free(entry);
            return;
        }
        current = &entry->next;
    }
}

void decrement_timer() {
    Timer* current = timer_list;
    while (current) {
        current->ticks--;
        if (current->ticks <= 0) {
            printf("Timeout for sequence number %d\n", current->seq_num);
            del_timer(current->seq_num);
        }
        current = current->next;
    }
}

void moveWindow(int* base) {
    Timer* current = timer_list;
    while (current) {
        if (current->ticks > 0) {
            *base = current->seq_num;
            return;
        }
        current = current->next;
    }
    // If no active timers, move base to the next expected sequence number
    *base += 1;
}