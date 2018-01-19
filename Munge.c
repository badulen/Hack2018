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
    int kill_percentage2;
    char tx1_mcast_dest[18];
    char tx2_mcast_dest[18];
    char rx_mcast_dest[18];
//    char rx_mcast_src[18];
    int tx1_port;
    int tx2_port;
    int rx_port;
    int tx1_sock;
    int tx2_sock;
    struct sockaddr_in tx1_sa;
    struct sockaddr_in tx2_sa;
    int tx1_salen;
    int tx2_salen;
    int rx_sock;
    struct sockaddr_in rx_sa;
    int rx_salen;
    struct ip_mreq mreq; //The ip_mreq structure is used with ICMPv2.
    struct ip_mreq_source mreq_source; //The ip_mreq_source structure is used with ICMPv3.

    enum {MODE_MCAST, MODE_SSM} input_mode;
    int yes = 1;
    int error = 0;
    char ifr[20];
    //char ifr[] = "eno1";
    input_mode = MODE_MCAST;
    int buf_index = 0;
    int packets_passed1 = 0;
    int packets_dropped1 = 0;  /* argv[1] = input filename */
    int packets_passed2 = 0;
    int packets_dropped2 = 0;  /* argv[1] = input filename */
 /* argv[2] = output filename */
    int randmax = 0;

    if (argc != 10)
    {
        printf("Usage: %s <rx port> <rx mcast addr> <tx1 port> <tx1 mcast addr> <tx2 port> <tx2 mcast addr> <net if name> <%% of datagrams to kill if1>  <%% of datagrams to kill if2> \n(PIDs in hex)\n", argv[0]);

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

    if (sscanf(argv[3], "%d", &tx1_port) != 1)
    {
        printf("Bad tx 1 port \"%s\" \n", argv[3]);
        exit(1);
    }
    printf("tx 1 port = %d \n", tx1_port);

    // set tx mcast group addr
    strncpy(tx1_mcast_dest, argv[4], 16);
    printf("tx1mcastdest = %s\n", tx1_mcast_dest);

   if (sscanf(argv[5], "%d", &tx2_port) != 1)
    {
        printf("Bad tx 2 port \"%s\" \n", argv[5]);
        exit(1);
    }
    printf("tx 2 port = %d \n", tx2_port);

    // set tx mcast group addr
    strncpy(tx2_mcast_dest, argv[6], 16);
    printf("tx2mcastdest = %s\n", tx2_mcast_dest);

   strncpy(ifr, argv[7], 16);

    if (sscanf(argv[8], "%d", &kill_percentage) != 1)
    {
        printf("Bad <%% of packets to kill> \"%s\"\n", argv[8]);
        exit(1);
    }
    printf("percentage bad = %d\n", kill_percentage);

    if (sscanf(argv[9], "%d", &kill_percentage2) != 1)
    {
        printf("Bad <%% of packets to kill> \"%s\"\n", argv[9]);
        exit(1);
    }
    printf("percentage bad 2 = %d\n", kill_percentage2);


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

    //tx1 socket
    if ((error = (tx1_sock = socket(AF_INET, SOCK_DGRAM, 0))) < 0)
    {
        printf("tx1 socket() creation failed with error %d\n", error);
        return 1;
    }

    setsockopt(tx1_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));


    if ((error = setsockopt(tx1_sock, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr))) < 0)
    {
        printf("tx1 setsockopt() failed with error %d\n", error);
        return 1;
    }

    tx1_sa.sin_family = AF_INET;
    tx1_sa.sin_port = htons((unsigned short)tx1_port);
    tx1_sa.sin_addr.s_addr = inet_addr(tx1_mcast_dest);
    tx1_salen = sizeof(tx1_sa);

    if ((error = bind(tx1_sock, (struct sockaddr*)&tx1_sa, sizeof(tx1_sa))) < 0)
    {
        printf("tx 1 bind() failed with error %d\n", error);
        close(tx1_sock);
        close(rx_sock);
        return 1;
    }


    //tx2 socket
    if ((error = (tx2_sock = socket(AF_INET, SOCK_DGRAM, 0))) < 0)
    {
        printf("tx2 socket() creation failed with error %d\n", error);
        return 1;
    }

    setsockopt(tx2_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));


    if ((error = setsockopt(tx2_sock, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr))) < 0)
    {
        printf("tx2 setsockopt() failed with error %d\n", error);
        return 1;
    }

    tx2_sa.sin_family = AF_INET;
    tx2_sa.sin_port = htons((unsigned short)tx2_port);
    tx2_sa.sin_addr.s_addr = inet_addr(tx2_mcast_dest);
    tx2_salen = sizeof(tx2_sa);

    if ((error = bind(tx2_sock, (struct sockaddr*)&tx2_sa, sizeof(tx2_sa))) < 0)
    {
        printf("tx 2 bind() failed with error %d\n", error);
        close(tx1_sock);
        close(tx2_sock);
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

        if (send_output)
        {
            sendto(tx1_sock, (char*)pbuf, dgramsize, 0, (struct sockaddr*)&tx1_sa, tx1_salen);
//            printf("dgramsize %d\n", dgramsize);
            packets_passed1++;
        }
        else
            packets_dropped1++;



        send_output = 1;
        randval = rand();

        if (((randval) < (kill_percentage2 * 214748) || (kill_percentage2 == 10000)))
            send_output = 0;

        if (send_output)
        {
            sendto(tx2_sock, (char*)pbuf, dgramsize, 0, (struct sockaddr*)&tx2_sa, tx2_salen);
//            printf("dgramsize %d\n", dgramsize);
            packets_passed2++;
        }
        else
            packets_dropped2++;

       buf_index++;
        if (buf_index == 128)
        {
            printf("TX 1: Passed %d dropped %d. TX 2: Passed %d dropped %d\n", packets_passed1, packets_dropped1,
		packets_passed2, packets_dropped2);
            buf_index = 0;
        }
    }

    return 0;
}


