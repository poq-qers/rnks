int timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y);
void checkInputSend(int argc, char* argv[]);
void checkInputRec(int argc, char* argv[]);
void setnonblocking(int* sock);