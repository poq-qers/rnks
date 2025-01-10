#define TIMEOUT_INTERVAL 3  // In "ticks" (e.g., 300ms per tick)
#define TIMEOUT 300

void add_timer(int seq_num, int ticks);
void del_timer(int seq_num);
void decrement_timer();
void moveWindow(int* base);