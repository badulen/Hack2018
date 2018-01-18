#include <iostream>

extern "C" {
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <time.h>
#include <net/if.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <cstring>
}

#include "ThreadSafeQueue.h"
#include <thread>
#include <set>

namespace
{
    const int RTP_PACKET_SIZE{1328};
    const int MAXBUFSIZE{65536};
    std::set<int> PacketRcvSet{};
}

// Define a Hackathon RTP packet
struct RtpHackPacket
{
    RtpHackPacket(unsigned char* data)
    {
        seqNumber = (data[2]<<8) + data[3];
        printf("\nSeq Number %d\n", seqNumber);
        std::memcpy(m_data, data, RTP_PACKET_SIZE);
    }

    int seqNumber;
    unsigned char* m_data[RTP_PACKET_SIZE];
};

static int sock = -1;
static int desiredRcvBufSize = 128 * 1024 * 1024;
static struct timespec start_time;
static unsigned long grabbed_bytes = 0;
//unsigned long grab_bytes = 10L * 1024L * 1024L;
unsigned long grab_bytes = 0;
unsigned char buffer[MAXBUFSIZE];
    ThreadSafeQueue<RtpHackPacket> RxQueue{};

//***********************************************************************************
// Helper Methods
//***********************************************************************************
static void SetRcvBufSize(int sock)
{
    int status;
    int rcvBufSize = 0;

    socklen_t rcvBufSizeLen = sizeof(rcvBufSize);
    if ((status = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void *)&rcvBufSize, &rcvBufSizeLen)) < 0)
    {
        perror("getsockopt() error for SO_RCVBUF");
    }
    else
    {
//        printf("Original SO_RCVBUF size: %d\n", rcvBufSize);
        if (rcvBufSize >= 2 * desiredRcvBufSize)
        {
            /* Success */
            return;
        }
    }

    rcvBufSize = desiredRcvBufSize;
    if ((status = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const void *)&rcvBufSize, sizeof(rcvBufSize))) < 0)
    {
        perror("setsockopt() error for SO_RCVBUF");
    }
    else
    {
        rcvBufSize = 0;
        rcvBufSizeLen = sizeof(rcvBufSize);
        if ((status = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void *)&rcvBufSize, &rcvBufSizeLen)) < 0)
        {
            perror("getsockopt() error for SO_RCVBUF");
        }
        else
        {
//            printf("SO_RCVBUF size: %d\n", rcvBufSize);
            if (rcvBufSize >= 2 * desiredRcvBufSize)
            {
                /* Success */
                return;
            }
        }
    }

    rcvBufSize = 128 * 1024 * 1024;
    if ((status = setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, (const void *)&rcvBufSize, sizeof(rcvBufSize))) < 0)
    {
        if (errno != EPERM)
        {
            perror("setsockopt() error for SO_RCVBUFFORCE");
        }
    }
    else
    {
        rcvBufSize = 0;
        rcvBufSizeLen = sizeof(rcvBufSize);
        if ((status = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void *)&rcvBufSize, &rcvBufSizeLen)) < 0)
        {
            perror("getsockopt() error for SO_RCVBUF");
        }
        else
        {
//            printf("SO_RCVBUF size: %d\n", rcvBufSize);
            if (rcvBufSize >= 2 * desiredRcvBufSize)
            {
                /* Success */
                return;
            }
        }
    }

    fprintf(stderr,
"Unable to set receive buffer size. If you continue you may experience missing\n"
"packets.\n\n"
"To fix this, you need to (change until reboot):\n\n"
"sudo sysctl -w net.core.rmem_max=%d\n\n"
"or (change permanently):\n\n"
"sudo sh -c 'echo \"net.core.rmem_max = %d\" > /etc/sysctl.d/60-udp-get.conf;service procps start'\n\n"
, desiredRcvBufSize, desiredRcvBufSize);
}

//***********************************************************************************
// Receiver Class
//***********************************************************************************
class Receiver
{
public:

    const char *listen_ip = "239.2.41.1";
    unsigned short listen_port = 1234;
    const char *ifceName = "enp1s0";
    Receiver( const char *listen_ip, unsigned short listen_port, const char *ifceName)
    {
        m_sock = -1;

        int status;
        int i;
        int dump_pid[0x2000] = {0};
        int do_dump = 0;
        int do_drop_nulls = 0;

        memset(dump_pid, 0, sizeof(dump_pid));

        // set content of struct saddr and imreq to zero

        memset(&saddr, 0, sizeof(struct sockaddr_in));
        memset(&imreq, 0, sizeof(struct ip_mreq));

        // open a UDP socket
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if ( m_sock < 0 )
        {
            perror("\nError creating socket\n");
            exit(-1);
        }

        int yes = 1;
        status = setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

        if (status < 0 )
        {
                printf("Error setting socket options\n\n");
                exit(1);
        }
        saddr.sin_family = PF_INET;
        saddr.sin_port = htons(listen_port);
        saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
        status = bind(m_sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));

        if ( status < 0 )
        {
            perror("\nError binding socket to interface\n");
            exit(-1);
        }

        imreq.imr_multiaddr.s_addr = inet_addr(listen_ip);
        imreq.imr_interface.s_addr = INADDR_ANY; // use DEFAULT interface

        if (strcmp(ifceName, "default") != 0)
        {
            struct ifaddrs *ifap;
            struct ifaddrs *ifa;

            if (getifaddrs(&ifap) != 0)
            {
                perror("getifaddrs() failed");
                close(m_sock);
                exit(-1);
            }

            for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next)
            {
                if ((ifa->ifa_addr->sa_family == AF_INET) && (strcmp(ifceName, ifa->ifa_name) == 0))
                {
                    imreq.imr_interface.s_addr = ((struct sockaddr_in*)(ifa->ifa_addr))->sin_addr.s_addr;
                    break;
                }
            }

            freeifaddrs(ifap);
            if (ifa == NULL)
            {
                fprintf(stderr, "\nInterface '%s' not found\n", ifceName);
                close(m_sock);
                exit(-1);
            }
        }


        // JOIN multicast group on default interface
        if ((status = setsockopt(m_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)&imreq, sizeof(struct ip_mreq))) < 0)
        {
            perror("setsockopt() error for IP_ADD_MEMBERSHIP");
            close(m_sock);
            exit(-1);
        }


        SetRcvBufSize(sock);

        socklen = sizeof(struct sockaddr_in);

        m_thread = std::thread{&Receiver::Start, this};
        m_thread.detach();
    }

    void Start()
    {
        printf("\nStarting Thread\n");

        printf("Listening for multicast packets on %s:%u\n", listen_ip, listen_port);
        while (true)
        {
            // receive packet from socket
            int status = recvfrom(m_sock, buffer, MAXBUFSIZE, 0,  (struct sockaddr *)&saddr, &socklen);

            if (status < 0)
            {
                printf("\nError reading data!\n");
                perror("recvfrom");
                exit(-1);
            }
            else if (status == 0)
            {
                // No data. Sleep for a bit, then try again
                printf("\nNo Data!\n");
                usleep(100);
            }
            else
            {
                printf("\nGot Data!\n");
                printf("\nRead %d bytes!\n", status);

                RtpHackPacket pkt{buffer};

                // If packet not present save it
                if (PacketRcvSet.count(pkt.seqNumber) == 0)
                {
                    printf("\nInserting pkt\n");
                    PacketRcvSet.insert(pkt.seqNumber);
                    RxQueue.Push(std::move(pkt));
                }
                else
                {
                    printf("\nAlready have pkt\n");
                }


    //                if ((do_drop_nulls) && ((status % 188) != 0))
    //                {
    //                    fprintf(stderr, "Incomplete TS packet received. Disabling null dropping\n");
    //                    do_drop_nulls = 0;
    //                }
    //
    //                if (do_drop_nulls)
    //                {
    //                    int pkt;
    //                    for (pkt = 0; pkt < status/188; pkt ++)
    //                    {
    //                        if (((buffer[pkt*188 + 2]) != 0xff) ||
    //                            ((buffer[pkt*188 + 1] & 0x1f) != 0x1f))
    //                        {
    //                            if (fwrite(&buffer[pkt * 188], 1, 188, fout) != 188)
    //                            {
    //                                perror("fwrite");
    //                                exit(-1);
    //                            }
    //                        }
    //                    }
    //                }
    //                else if (fwrite(buffer, 1, status, fout) != (unsigned)status)
    //                {
    //                    perror("fwrite");
    //                    exit(-1);
    //                }
    //
    //                fflush(fout);
    //
    //                if (grabbed_bytes == 0)
    //                {
    //                    if (fout != stdout)
    //                    {
    //                        printf("Grabbing.");
    //                        fflush(stdout);
    //                    }
    //                }
    //                else if ((grabbed_bytes & 0xfff00000) != ((grabbed_bytes + status) & 0xfff00000))
    //                {
    //                    if (fout != stdout)
    //                    {
    //                        printf(".");
    //                        fflush(stdout);
    //                    }
    //                }
    //            grabbed_bytes += status;
            }
        }
    }

private:

    std::thread m_thread;
    int m_sock;
    struct sockaddr_in saddr;
    struct ip_mreq imreq;
    socklen_t socklen;
};

int main()
{
    printf("\nStarting RX script\n");

    printf("\nCreating Receiver 1\n");
    Receiver rxOne{"239.2.41.1", 1234, "enp1s0"};
    Receiver rxTwo{"239.2.41.1", 1234, "enp1s0"};

    rxOne.Start();
    rxTwo.Start();

    return 0;
}

