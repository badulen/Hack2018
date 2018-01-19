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

/*--------------------------------------------------------------------------------------*
 *   NAME         : main                                                                *
 *   RETURNS      : 0                                                                   *
 *   PARAMS       : argc, argv:                                                         *
 *   DESCRIPTION  : entry point                                                         *
 *--------------------------------------------------------------------------------------*/
int main(int argc, char* argv[])
{
    int dgramsize = 0;
    unsigned char buf[1500*128];
    int kill_percentage;
    char tx_mcast_dest[18];
    char rx_mcast_dest[18];
//    char rx_mcast_src[18];
    int tx_port;
    int rx_port;
    int tx_sock;
    struct sockaddr_in tx_sa;
    int tx_salen;
    int rx_sock;
    struct sockaddr_in rx_sa;
    int rx_salen;
    struct ip_mreq mreq; //The ip_mreq structure is used with ICMPv2.
    struct ip_mreq_source mreq_source; //The ip_mreq_source structure is used with ICMPv3.

    enum {MODE_MCAST, MODE_SSM} input_mode;
    int yes = 1;
    int error = 0;
    char ifr[] = "eth1";
    //char ifr[] = "eno1";
    input_mode = MODE_MCAST;
    int buf_index = 0;
    int packets_passed = 0;
    int packets_dropped = 0;  /* argv[1] = input filename */
  /* argv[2] = output filename */
    int randmax = 0;

    if (argc != 6)
    {
        printf("Usage: %s <rx port> <rx mcast addr> <rx ssm mcast source> <tx1 port> <tx1 mcast addr> <%% of datagrams to kill>  \n(PIDs in hex)\n", argv[0]);

        exit(1);
    }

    if (sscanf(argv[1], "%d", &rx_port) != 1)
    {
        printf("Bad port \"%s\" \n", argv[1]);
        exit(1);

    }
    printf("rx port = %d \n", rx_port);

    // set rx mcast group addr
    strncpy(rx_mcast_dest, argv[2], 16);
    printf("rxmcastdest = %s\n", rx_mcast_dest);

    // set rx mcast ssm src addr
//    strncpy(rx_mcast_src, argv[3], 16);
//        printf("rxmcastsrc = %s\n", rx_mcast_src);

    if (sscanf(argv[3], "%d", &tx_port) != 1)
    {
        printf("Bad tx port \"%s\" \n", argv[3]);
        exit(1);
    }
    printf("tx port = %d \n", tx_port);

    // set tx mcast group addr
    strncpy(tx_mcast_dest, argv[4], 16);
    printf("txmcastdest = %s\n", tx_mcast_dest);


    if (sscanf(argv[5], "%d", &kill_percentage) != 1)
    {
        printf("Bad <%% of packets to kill> \"%s\"\n", argv[5]);
        exit(1);
    }
    printf("percentage bad = %d\n", kill_percentage);

    //rx socket
    if ((error = (rx_sock = socket(AF_INET, SOCK_DGRAM, 0))) < 0)
    {
        printf("rx socket() creation failed with error %d\n", error);
        return 1;
    }


    if ((error = setsockopt(rx_sock, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr))) < 0)
    {
        printf("rx setsockopt() SO_BINDTODEVICE failed with error %d\n", error);
        return 1;
    }
    if ((error = setsockopt(rx_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) < 0)
    {
        printf("rx setsockopt() SO_REUSEADDR failed with error %d\n", error);
    }


    rx_sa.sin_family = AF_INET;
    rx_sa.sin_port = htons((unsigned short)rx_port);
    rx_sa.sin_addr.s_addr = INADDR_ANY;

    if ((error = bind(rx_sock, (struct sockaddr*)&rx_sa, sizeof(rx_sa))) < 0)
    {
        printf("rx bind() failed with error %d\n", error);
        close(rx_sock);
        return 1;
    }

    //tx socket
    if ((error = (tx_sock = socket(AF_INET, SOCK_DGRAM, 0))) < 0)
    {
        printf("tx socket() creation failed with error %d\n", error);
        return 1;
    }

    setsockopt(tx_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));


    if ((error = setsockopt(tx_sock, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr))) < 0)
    {
        printf("tx setsockopt() failed with error %d\n", error);
        return 1;
    }

    tx_sa.sin_family = AF_INET;
    tx_sa.sin_port = htons((unsigned short)tx_port);
    tx_sa.sin_addr.s_addr = inet_addr(tx_mcast_dest);
    tx_salen = sizeof(tx_sa);

    if ((error = bind(tx_sock, (struct sockaddr*)&tx_sa, sizeof(tx_sa))) < 0)
    {
        printf("tx bind() failed with error %d\n", error);
        close(tx_sock);
        close(rx_sock);
        return 1;
    }


    if (input_mode == MODE_MCAST)
    {
            //Join multicast group
        mreq.imr_multiaddr.s_addr = inet_addr(rx_mcast_dest);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if ((error = setsockopt(rx_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq,
            sizeof(struct ip_mreq))) < 0)
        {
            printf("setsockopt - IP_ADD_MEMBERSHIP Error %d", error);
            exit(1);
        }
    }
    else if (input_mode == MODE_SSM)
    {
        //Join SSM multicast group
        //or use IP_ADD_SOURCE_MEMBERSHIP for SSM
//        mreq_source.imr_multiaddr.s_addr = inet_addr(rx_mcast_dest);
//        mreq_source.imr_sourceaddr.s_addr = inet_addr(rx_mcast_src);
//        mreq_source.imr_interface.s_addr = htonl(INADDR_ANY);
//        if ((error = setsockopt(rx_sock, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, (char*)&mreq_source,
//            sizeof(struct ip_mreq_source))) < 0)
//        {
//            printf("setsockopt - IP_ADD_MEMBERSHIP Error %d", error);
//            exit(1);
//        }
    }

    rx_salen = sizeof(rx_sa);

    printf("Ready to receive\n");

    //copy some packets to the output
    while(1)
    {
        int send_output = 0;
        int randval;

        char* pbuf = buf + buf_index*1500;

        dgramsize = recvfrom(rx_sock, (char*)pbuf, 1500, 0, (struct sockaddr*)&rx_sa, &rx_salen);

//        printf("seq %x%x\n", pbuf[2], pbuf[3]);

        send_output = 1;
        randval = rand();

        if (((randval) < (kill_percentage * 214748) || (kill_percentage == 10000)))
            send_output = 0;

        if (send_output) {
            sendto(tx_sock, (char*)pbuf, dgramsize, 0, (struct sockaddr*)&tx_sa, tx_salen);

//        printf(".");
//            printf("dgramsize %d\n", dgramsize);
            packets_passed++;
        }
        else
            packets_dropped++;

        buf_index++;
        if (buf_index == 128)
        {
            printf("Packets Passed %d dropped %d\n", packets_passed, packets_dropped);
            buf_index = 0;
        }
    }

    return 0;
}

