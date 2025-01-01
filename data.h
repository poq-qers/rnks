struct request {
    unsigned char ReqType;
    #define ReqHello 'H'
    #define ReqData 'D'
    #define ReqClose 'C'
    long FlNr;/* Data length (text line) in Byte ;
    //if it’s a Hello packet we can reuse it for transmit the window size*/
    unsigned long SeNr;/* Sequence Number (== 0 with begin of file) */
    #define PufferSize 256
    char name[PufferSize];
    int recNr;  // unique number for different receiver on Loopback
                // interface (eg. only set for retransmission, else zero)
};

struct answer {
    unsigned char AnswType;
    #define AnswHello 'H'
    #define AnswNACK 'N' //Multicast Group receiver send
    //negative acknoledgement if received packet is not in order or Timeout
    #define AnswClose 'C'
    unsigned SeNo;
    int recNr; // Random number for different receiver on Loopback, can be
               // provided as program argument
};

struct networkContainer {
    // Allgemein
    int socket;
    const char* multicast_address;
    int port;
    int window_size;
    struct sockaddr_in6 my_addr;
    struct sockaddr_in6 remote_addr;
    int rec_numbers[10];
    const char* filename;

    // Receiver
    int serialID;
    int error_packets;
};