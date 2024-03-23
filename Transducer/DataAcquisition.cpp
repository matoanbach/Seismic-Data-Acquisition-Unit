#include "DataAcquisition.h"
using namespace std;

DataAc *DataAc::instance = nullptr;

static void interruptHandler(int signum)
{
    switch (signum)
    {
    case SIGINT:
        DataAc::instance->shutdown();
        break;
    }
}

DataAc::DataAc()
{
    is_running = false;
    ShmPTR = nullptr;
    DataAc::instance = this;
}
void DataAc::shutdown()
{
    cout << "Acquisition Unit::shutdown" << endl;
    is_running = false;
}

int DataAc::run()
{
    // init the signal handler with SIGINT
    action.sa_handler = interruptHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);

    // init a shared memory key
    ShmKey = ftok(MEMNAME, 65);
    // attach the shared memory key to the a piece of memory in RAM
    ShmID = shmget(ShmKey, sizeof(struct SeismicMemory *), IPC_CREAT | 0666);
    if (ShmID < 0)
    {
        cout << "Acquisition Unit: shmget() error" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }

    // attach a pointer to that piece of memory
    ShmPTR = (struct SeismicMemory *)shmat(ShmID, NULL, 0);
    if (ShmPTR == (void *)-1)
    {
        cout << "Acquisition Unit: shmat() error" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }

    // open a semaphore
    sem_id1 = sem_open(SEMNAME, O_CREAT, SEM_PERMS, 0);
    is_running = true;

    // init a socket
    int len, ret;

    socklen_t addrlen = sizeof(cliaddr);
    const int PORT = 1153;              // Address of the server
    const char IP_ADDR[] = "127.0.0.1"; // Address of the server
    int myport;
    char buf[BUF_LEN];

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        cout << "Cannot create the socket" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }

    memset((char *)&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    ret = inet_pton(AF_INET, IP_ADDR, &myaddr.sin_addr);
    if (ret == 0)
    {
        cout << "No such address" << endl;
        cout << strerror(errno) << endl;
        close(fd);
        return -1;
    }
    myaddr.sin_port = htons(PORT);

    ret = bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr));
    if (ret < 0)
    {
        cout << "Cannot bind the socket to the local address" << endl;
        cout << strerror(errno) << endl;
        return -1;
    }
    else
    {
#ifdef DEBUG
        cout << "socket fd:" << fd << " bound to address " << inet_ntoa(myaddr.sin_addr) << endl;
#endif
    }

    memset((char *)&cliaddr, 0, sizeof(cliaddr));
    cliaddr.sin_family = AF_INET;
    ret = inet_pton(AF_INET, IP_ADDR, &cliaddr.sin_addr);
    if (ret == 0)
    {
        cout << "No such address" << endl;
        cout << strerror(errno) << endl;
        close(fd);
        return -1;
    }

    // read data from the transducer
    ret = pthread_create(&rcvtid, NULL, recv_func, this);
    ret = pthread_create(&sndtid, NULL, send_func, this);
    pthread_mutex_init(&lock_x1, NULL);
    pthread_mutex_init(&lock_x2, NULL);
    // ret = pthread_create(&sndtid, NULL, send_func, this);

    char message[BUF_LEN];

    while (is_running)
    {
        // Semaphores might not be necessary
        // Check if the data is written already on the other end
        if (ShmPTR->seismicData->status == WRITTEN)
        {
            // decrement the semaphore
            Message msg;
            // sem_wait(sem_id1);
            msg.portNo = ShmPTR->packetNo;
            msg.len = ShmPTR->seismicData->packetLen;
            strncpy(msg.data, ShmPTR->seismicData->data, BUF_LEN);
            // increment the semaphore
            // sem_post(sem_id1);

            // push msg to the queue
            // lock the queue
            pthread_mutex_lock(&lock_x1);
            dataPacket.push(msg);
            // unlock the queue
            pthread_mutex_unlock(&lock_x1);
        }
        sleep(1);
    }

    sem_close(sem_id1);
    sem_unlink(SEMNAME);

    pthread_join(rcvtid, NULL);
    pthread_join(sndtid, NULL);

    shmdt((void *)ShmPTR);
    shmctl(ShmID, IPC_RMID, NULL);
    cout << "Unit: DONE" << endl;
    return 0;
}

void *send_func(void *arg)
{
    DataAc *dataAc = (DataAc *)arg;
    dataAc->SendFunction();
    pthread_exit(NULL);
}
void DataAc::SendFunction()
{
    char message[BUF_LEN];
    int ret;
    while (is_running)
    {
        // Semaphores might not be necessary
        // Check if the data is written already on the other end
        if (dataPacket.size() == 0)
            sleep(1);
        else
        {
            // pop msg to the queue
            // lock the queue
            pthread_mutex_lock(&lock_x1);
            Message msg = dataPacket.front();
            dataPacket.pop();
            // unlock the queue
            pthread_mutex_unlock(&lock_x1);
            // decrement the semaphore
            cout << "packet size: " << dataPacket.size() << "  active clients: " << clients.size() << "  rogue client: " << rogueclients.size() << endl;
            pthread_mutex_lock(&lock_x2);
            for (auto it = clients.begin(); it != clients.end(); it++)
            {
                inet_pton(AF_INET, it->second.IPAddress, &myaddr.sin_addr);
                cliaddr.sin_port = htons(it->second.port);
                ret = sendto(fd, (char *)&msg, sizeof(msg.data), 0, (struct sockaddr *)&cliaddr, sizeof(cliaddr));
                cout << "send_func: packet sent to " << it->second.IPAddress << ":" << it->second.port << endl;
            }
            pthread_mutex_unlock(&lock_x2);

            // increment the semaphore
        }
        sleep(1);
    }
    pthread_exit(NULL);
}

void *recv_func(void *arg)
{
    DataAc *dataAc = (DataAc *)arg;
    dataAc->ReceiveFunction();
    pthread_exit(NULL);
}

void DataAc::ReceiveFunction()
{
    char buf[BUF_LEN];
    struct sockaddr_in recvaddr;
    socklen_t addrlen = sizeof(recvaddr);

    while (is_running)
    {
        memset(buf, 0, BUF_LEN);
        int len = recvfrom(fd, buf, BUF_LEN, 0, (struct sockaddr *)&recvaddr, &addrlen);
        if (len > 0)
        {
            // ignore any client in the rogue list here
            int port = ntohs(recvaddr.sin_port);

            if (rogueclients.find(port) != rogueclients.end())
            {
                continue;
            }

            // extract the data "Subscribe,DataCenter%d,Leaf"
            //                          "Cancel,DataCenter%d"

            int len = strlen(buf);
            int index = 0;
            char request[BUF_LEN];
            char username[BUF_LEN];
            char password[BUF_LEN];
            char clientNo;

            char *bufPtr = &buf[0];
            while (bufPtr[index] != ',' && bufPtr[index])
                ++index;
            memcpy(request, &bufPtr[0], index);
            bufPtr = &buf[index + 1];

            while (bufPtr[index] != ',' && bufPtr[index])
                ++index;
            memcpy(username, &bufPtr[0], index - 1);
            clientNo = bufPtr[index - 1];
            memcpy(password, &bufPtr[index + 1], len - index - 1);

            // make a new client
            Client client;
            strncpy(client.username, username, sizeof(username));
            inet_ntop(AF_INET, &recvaddr.sin_addr, client.IPAddress, INET_ADDRSTRLEN);
            client.port = port;

            // ignore any client trying to spam data
            timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            map<int, timespec>::iterator it;
            if ((it = requestHistory.find(port)) != requestHistory.end())
            {
                // this client has requested before -> calculate the request time
                double diff = (double)(now.tv_sec - it->second.tv_sec) * nanosecsPerSecond + (now.tv_nsec - it->second.tv_nsec);
                diff /= nanosecsPerSecond;

                if (diff < timeRequestLimit) // request too fast -> straight to the rogue list
                {
                    if (clients.find(client.port) != clients.end())
                    {
                        clients.erase(clients.find(client.port));
                    }

                    rogueclients[client.port] = client;
                    cout << "recv_func: " << client.IPAddress << ":" << client.port << " to the rogue client list" << endl;
                    continue;
                }
            }
            // give this client a new time stamp
            requestHistory[client.port] = now;

            if (strncmp(request, "Subscribe", 9) == 0)
            {
                // check password brute force here
                if (strncmp(password, PASSWORD, PASSWORD_LENGTH) == 0)
                {
                    pthread_mutex_lock(&lock_x2);
                    auto cl = clients.find(client.port);
                    if (cl != clients.end())
                    {
                        cout << "recv_func: " << cl->second.IPAddress << ":" << cl->second.port << " has already subscribed" << endl;
                    }
                    else
                    {
                        clients[client.port] = client;
                        // forgive their sins in the past
                        passwordHistory[client.port] = 0;
                        cout << "recv_func: " << client.IPAddress << client.port << " subscribed" << endl;
                    }
                    pthread_mutex_unlock(&lock_x2);
                }
                else
                {
                    map<int, int>::iterator it;
                    // update the password guesses
                    passwordHistory[client.port] = (((it = passwordHistory.find(client.port)) != passwordHistory.end()) ? it->second++ : 1);
                    // clients have 3 gueses
                    if (passwordHistory[client.port] >= 3)
                    {
                        if (clients.find(client.port) != clients.end())
                        {
                            clients.erase(clients.find(client.port));
                        }
                        rogueclients[client.port] = client;
                        cout << "recv_func: " << client.IPAddress << ":" << client.port << " to the rogue client list" << endl;
                    }
                }
            }
            else if (strncmp(request, "Cancel", 6) == 0)
            {
                pthread_mutex_lock(&lock_x2);
                clients.erase(clients.find(client.port));
                pthread_mutex_unlock(&lock_x2);
            }
            else
            {
                cout << "recv_func: unknown command " << request << endl;
            }
            sleep(1);
        }
    }
    pthread_exit(NULL);
}

void DataAc::strip()
{
    return;
}