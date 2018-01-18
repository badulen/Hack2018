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
#include <stdint.h>

namespace
{
    const int RTP_PACKET_SIZE{1328};
    const int MAXBUFSIZE{65536};

    const char *multicast_ip = "239.99.1.1";
    short multicast_port = 5000;

}

// Define a Hackathon RTP packet
struct RtpHackPacket
{
    RtpHackPacket(unsigned char* data)
    {
        seqNumber = (data[2]<<8) + data[3];
//        printf("\nSeq Number %d\n", seqNumber);
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


        // JOIN multicast group on default interface
        if ((status = setsockopt(m_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)&imreq, sizeof(struct ip_mreq))) < 0)
        {
            perror("setsockopt() error for IP_ADD_MEMBERSHIP");
            close(m_sock);
            exit(-1);
        }


        SetRcvBufSize(m_sock);

        socklen = sizeof(struct sockaddr_in);

        printf("Listening for multicast packets on %s:%u\n", listen_ip, listen_port);
    }

    void Start()
    {
        m_thread = std::thread{&Receiver::Execute, this};
        printf("Sending Packets on %s:%u\n", multicast_ip, multicast_port);
    }

    void Execute()
    {
        printf("\nStarting Thread\n");

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
 //               printf("\nGot Data!\n");
 //               printf("\nRead %d bytes!\n", status);

                RtpHackPacket pkt{buffer};

                // If packet not present save it
                if (RxQueue.Count(pkt.seqNumber) == 0)
                {
//                    printf("\nInserting pkt\n");
                    RxQueue.Push(std::move(pkt));
                }
                else
                {
//                    printf("\nAlready have pkt\n");
                }
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

//***********************************************************************************
// Playout Class
//***********************************************************************************
class Player
{
public:

    Player()
    {
        FILE *fin;
        unsigned char pkt[188];
        unsigned int pid_count[8192];
        long long first_pcr27mhz = -1;
        long first_pcrpos = -1;
        unsigned int calc_pid = 0xffff;
        int status;
        struct in_addr iaddr;
        const unsigned char ttl = 3;
        const unsigned char one = 1;
        long long total_sent = 0;
        int loop_count = 0;
        int i;
        long filesize = 0;

        multicast_ip = "239.32.32.32";
        multicast_port = 1234;
        const char *ifceName = "enp1s0";

        memset(pid_count, 0, sizeof(pid_count));

        // set content of struct saddr and imreq to zero
        memset(&saddr, 0, sizeof(struct sockaddr_in));
        memset(&iaddr, 0, sizeof(struct in_addr));

        // open a UDP socket
        m_sock = socket(PF_INET, SOCK_DGRAM, 0);
        if (m_sock < 0)
        {
            perror("Error creating socket");
            exit(-1);
        }

    int yes = 1;
    status = setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    if (status < 0 )
    {
            printf("Error setting socket options\n\n");
            exit(1);
    }

    /* Bind to particular interface only (e.g. eth1) */
    if ((status = setsockopt(m_sock, SOL_SOCKET, SO_BINDTODEVICE, ifceName, strlen(ifceName))) < 0)
    {
        perror("setsockopt() error for SO_BINDTODEVICE");
        printf("%s\n", strerror(errno));
        close(sock);
        exit(-1);
    }

    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(0); // Use the first free port
    saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
    status = bind(m_sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));

    if ( status < 0 )
    {
        perror("Error binding socket to interface");
        exit(0);
    }

    iaddr.s_addr = htonl(INADDR_ANY); // use DEFAULT interface

    // Set the outgoing interface to DEFAULT
    status =setsockopt(m_sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr, sizeof(struct in_addr));
    if (status < 0)
    {
        printf("Failed to set sock opt");
        exit(0);
    }

    // Set multicast packet TTL to 3; default TTL is 1
    status =setsockopt(m_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(unsigned char));
    if (status < 0)
    {
        printf("Failed to set sock opt");
        exit(0);
    }

    // send multicast traffic to myself too
    status = setsockopt(m_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(unsigned char));
    if (status < 0)
    {
        printf("Failed to set sock opt");
        exit(0);
    }

    // set destination multicast address
    saddr.sin_family = PF_INET;
    saddr.sin_addr.s_addr = inet_addr(multicast_ip);
    saddr.sin_port = htons(multicast_port);

//    // sync byte                        8   0x47
//    // Transport Error Indicator (TEI)  1   Set by demodulator if can't correct errors in the stream, to tell the demultiplexer that the packet has an uncorrectable error [11]
//    // Payload Unit Start Indicator     1   1 means start of PES data or PSI otherwise zero only.
//    // Transport Priority               1   1 means higher priority than other packets with the same PID.
//    // PID                              13  Packet ID
//    // Scrambling control               2   '00' = Not scrambled.   The following per DVB spec:[12]   '01' = Reserved for future use,   '10' = Scrambled with even key,   '11' = Scrambled with odd key
//    // Adaptation field exist           2   01 = no adaptation fields, payload only, 10 = adaptation field only, 11 = adaptation field and payload
//    // Continuity counter               4   Incremented only when a payload is present (i.e., adaptation field exist is 01 or 11)[13]
//    // Adaptation field                 0 or more   Depends on flags
//    // Payload Data                     0 or more   Depends on flags
//
//    // Adaptation Field Length                  8   Number of bytes in the adaptation field immediately following this byte
//    // Discontinuity indicator                  1   Set to 1 if current TS packet is in a discontinuity state with respect to either the continuity counter or the program clock reference
//    // Random Access indicator                  1   Set to 1 if the PES packet in this TS packet starts a video/audio sequence
//    // Elementary stream priority indicator     1   1 = higher priority
//    // PCR flag                                 1   1 means adaptation field does contain a PCR field
//    // OPCR flag                                1   1 means adaptation field does contain an OPCR field
//    // Splicing point flag                      1   1 means presence of splice countdown field in adaptation field
//    // Transport private data flag              1   1 means presence of private data bytes in adaptation field
//    // Adaptation field extension flag          1   1 means presence of adaptation field extension
//
//    // PCR                                      33+9    Program clock reference, stored in 6 octets in big-endian as 33 bits base, 6 bits padding, 9 bits extension.
//    // OPCR                                     33+9    Original Program clock reference. Helps when one TS is copied into another
//    // Splice countdown                         8   Indicates how many TS packets from this one a splicing point occurs (may be negative)
//    // stuffing bytes                           variable
//

    }

    void Start()
    {
        m_thread = std::thread{&Player::Execute, this};
        printf("Sending Packets on %s:%u\n", multicast_ip, multicast_port);
    }

    void Execute()
    {
        printf("\nStarting Playout Thread\n");

        while(true)
//        for (int loop_count = 0; true; loop_count ++)
        {
            static int seqPlay{0};
//            if (RxQueue.Count(seqPlay))
//            {
                auto pkt = RxQueue.WaitAndPop();
                seqPlay++;
  //              printf("\nFound packet to play! %d, %d\n", seqPlay, pkt.seqNumber);
                socklen_t socklen = sizeof(struct sockaddr_in);

                int status = sendto(m_sock, pkt.m_data, RTP_PACKET_SIZE, 0, (struct sockaddr *)&saddr, socklen);
                if (status < 0)
                {
                    perror("sendto() error");
                    printf("%s\n", strerror(errno));
                }
                else
                {
//                    printf("\nPalyed Packet\n");
                }


//            }
//            else
//            {
//                printf("\nCan't play, wait\n");
//                sleep(10);
//            }
        }
//        if ((loop_count == 0) && (! do_calc_rate))
//        {
//            printf("Starting to transmit on rtp://%s:%u\n", multicast_ip, multicast_port);
//            clock_gettime(CLOCK_MONOTONIC, &start_time);
//        }
//
//        if (fseek(fin, 0, SEEK_SET)) // seek back to beginning of file
//        {
//            perror("fseek");
//            exit(-1);
//        }
//
//        while (!feof(fin))
//        {
//            int ch = fgetc(fin);
//            if (ch < 0)
//            {
//                // EOF
//                break;
//            }
//
//            if (ch != 0x47)
//            {
//                unsigned int pos = ftell(fin) - 1;
//                unsigned int pos2;
//
//                printf("WARNING: TS Sync Byte incorrect. Expected 0x47. Got 0x%02x (at %u)\n", (pkt[0]&0xff), pos);
//
//                while (ch >= 0 && ch != 0x47)
//                {
//                    ch = fgetc(fin);
//                }
//                pos2 = ftell(fin) - 1;    // Position of sync byte
//                printf("Skipped %u bytes\n", pos2 - pos);
//
//                if (ch < 0)
//                {
//                    // EOF
//                    break;
//                }
//                pos = pos2;
//            }
//
//            pkt[0] = ch;
//            if (fread(pkt+1, 1, 187, fin) != 187)
//            {
//                printf("EOF\n");
//                continue;
//            }
//
//            unsigned int pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
//
//            pid_count[pid]++;
//
//            if (send_packet(sock, pkt, (struct sockaddr *)&saddr, socklen))
//            {
//                rate_limit(total_sent + fpos);
//            }
//        }
//    }
    }

private:

    std::thread m_thread;
    int m_sock;
    struct sockaddr_in saddr;
    struct ip_mreq imreq;
    socklen_t socklen;
};


//***********************************************************************************
// Main
//***********************************************************************************
int main()
{
    printf("\nStarting RX script\n");

    printf("\nCreating Receiver 1\n");
    Receiver rxOne{"239.2.41.1", 1234, "enp1s0"};
    Receiver rxTwo{"239.2.41.1", 1234, "enp1s0"};

    rxOne.Start();
    rxTwo.Start();

    printf("\nCreating Player 1\n");
    Player txOne{};
    txOne.Start();

    while(true)
    {
    }
    return 0;
}

