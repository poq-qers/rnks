#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>

// Timer Node Structure
typedef struct TimerNode {
    int ticks;
    int seq_num;
    struct TimerNode* next;
} TimerNode;

TimerNode* timer_list = NULL;  // Head of the timer list

// Add a timer to the list
void add_timer(int seq_num, int ticks) {
    TimerNode* new_node = (TimerNode*)malloc(sizeof(TimerNode));
    new_node->seq_num = seq_num;
    new_node->ticks = ticks;
    new_node->next = NULL;

    if (timer_list == NULL || ticks < timer_list->ticks) {
        if (timer_list != NULL) {
            timer_list->ticks -= ticks;
        }
        new_node->next = timer_list;
        timer_list = new_node;
        return;
    }

    TimerNode* current = timer_list;
    while (current->next != NULL && ticks >= current->next->ticks) {
        ticks -= current->next->ticks;
        current = current->next;
    }

    new_node->ticks = ticks;
    new_node->next = current->next;
    if (current->next != NULL) {
        current->next->ticks -= ticks;
    }
    current->next = new_node;
}

// Delete a timer from the list
void del_timer(int seq_num) {
    TimerNode* current = timer_list;
    TimerNode* prev = NULL;

    while (current != NULL && current->seq_num != seq_num) {
        prev = current;
        current = current->next;
    }

    if (current == NULL) return;  // Timer not found

    if (prev == NULL) {
        timer_list = current->next;
    } else {
        prev->next = current->next;
    }

    if (current->next != NULL) {
        current->next->ticks += current->ticks;
    }

    free(current);
}

// Decrement the first timer
void decrement_timer() {
    if (timer_list == NULL) return;

    timer_list->ticks--;
    if (timer_list->ticks <= 0) {
        printf("Timeout for packet %d\n", timer_list->seq_num);

        TimerNode* expired = timer_list;
        timer_list = timer_list->next;
        if (timer_list != NULL) {
            timer_list->ticks += expired->ticks;
        }
        free(expired);
    }
}

void moveWindow(int* base) {
    while (timer_list != NULL && timer_list->seq_num == *base) {
        del_timer(*base);
        (*base)++;
    }
}