// DataAcquisition.h - Class declaration for the transducer

// Ma Toan Bach

#ifndef _DATAACQUISITION_H_
#define _DATAACQUISITION_H_

#include <errno.h>
#include <iostream>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <queue>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "SeismicData.h"
#include <sys/time.h>
#include <map>
#include <iterator>

using namespace std;

static void interruptHandler(int signum);
void *recv_func(void *arg);
void *send_func(void *arg);
const long nanosecsPerSecond = 1000000000;
const double timeRequestLimit = 2.0; // second
const char PASSWORD[] = "Leaf";      // image this password is hidden in an env file
const int PASSWORD_LENGTH = 4;

typedef struct client
{
    char username[4096];
    char IPAddress[4096];
    int port;
} Client;

typedef struct message
{
    int portNo;
    int len;
    char data[BUF_LEN];
} Message;

class DataAc
{
    queue<Message> dataPacket;

    // variables used to communicate with the transducer
    bool is_running;
    sem_t *sem_id1;
    key_t ShmKey;
    int ShmID;
    struct SeismicMemory *ShmPTR;

    // variables used to communicate with clients
    // a list of subscribers
    struct sockaddr_in myaddr;
    struct sockaddr_in cliaddr;
    map<uint16_t, Client> clients;
    map<uint16_t, Client> rogueclients;
    map<int, timespec> requestHistory;
    map<int, int> passwordHistory;

    int fd;
    // interupt signal handler
    struct sigaction action;
    // threads
    pthread_t rcvtid;
    pthread_t sndtid;
    pthread_mutex_t lock_x1; // locker for dataPacket
    pthread_mutex_t lock_x2; // locker for client list

public:
    DataAc();
    int run();
    void shutdown();
    void ReceiveFunction();
    void SendFunction();
    void strip();
    static DataAc *instance;
};

#endif // _DATAACQUISITION_H_