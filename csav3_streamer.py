################################################################################
# (C) Ericsson AB. All Rights Reserved.
#
# P R O P R I E T A R Y & C O N F I D E N T I A L
#
# The contents of this document may not be used for commercial purposes;
# copied; disclosed; reproduced; stored in a retrieval system or transmitted
# in any form or by any means, whether in whole or in part, without prior
# written agreement.
#
# ##############################################################################
# Project Name      : CSAv3 NAB Demo
# File Name         : csav3_streamer.py
# Author            :
# Date Originated   :
#
# Description : Python script to extract information from a video stream and
#               process it. The script can read from file/socket and send the elbarated
#               stream to file/socket.
#
# Change History ...
# 20170208     R. Kelsey    copied in
# 20170209     ealafit      Modified and checked in - clearly a work in progress.
# 20170224     eeltbeg      Rewritten from scratch. Created read/write file classes and
#                           read/write socket classes.
# 20170301     eeltbeg      Improved exception catching. Made small modification to make the code
#                           compatible with python 3.3
# 20170315     eeltbeg      Following ealafit suggestion, removed all the queues and replaced with
#                           pipes which are performant
# 20170315     ealafit      Added PCIe communication to/from the KC705 card
# 20170317     eeltbeg      Fixed the exception raised during the exit phase. Added the CC checker
# 20170320     eeltbeg      Fixed the exception raised during the exit phase. Added the CC checker
#
# ##############################################################################
# Code to make this script run in python2
# Requires the "future" module:
# > sudo apt-get install python-pip
# > sudo pip-install future
from __future__ import print_function
from timeit import default_timer as timer
import queue
import socket
import multiprocessing as mp
import sys
import struct
import logging
import json
import time
import signal


# Import PCIe access package (links to C code)
# import pcie

logger = logging.getLogger(__name__)



NR_PKT_TEST_BPS = 3048
DATA_CHUNK_IN_BYTES = 188*70    # Chunk of data to be read from File/Socket
DO_DECRYPTION = False
QUEUE_SIZE = 500
ENABLE_PCIE = False
DEBUG = True
BYPASS = True

CSI="\x1B["
R_CLR = CSI + "91m"
G_CLR = CSI + "92m"
Y_CLR = CSI + "93m"
D_CLR = CSI + "95m"
RST_CLR=CSI+"m"



#deal with socket lib not supporting SSMs
#windows needs 15
#linux needs 39
if not hasattr(socket, 'IP_ADD_SOURCE_MEMBERSHIP'):
    if sys.platform.lower().startswith('win'):
        #WINDOWS
        setattr(socket, 'IP_ADD_SOURCE_MEMBERSHIP', 15)
    elif sys.platform.lower().startswith('linux'):
        #LiNUX
        setattr(socket, 'IP_ADD_SOURCE_MEMBERSHIP', 39)
    else:
        logging.debug("\tThis operating system is not supported\n\n")
        sys.exit(9)





def print_ts(ts_pkt):

    key = 0
    j=0
    is_fpga_format = False

    if len(ts_pkt) > 188:   #FPGA formatted TS packet
        s_pid = "(TS packet FPGA format)"
        is_fpga_format = True

        for i in reversed(range(16)):     # 128 bit key
            key = key + (int(ts_pkt[j])<<(i*8))
            j +=1

        payload_start = ts_pkt[22]
        odd_even      = (ts_pkt[23]>>1) & 0x1
        scrambling_en = ts_pkt[23] & 0x1
        indx_offs     = 32
        adap_field_ctrl = (ts_pkt[indx_offs+3] & 0x30) >> 4   # bits 5 and 4
        if adap_field_ctrl == 0x1:
            s_adap = " (payload only)"
        elif adap_field_ctrl == 0x2:
            s_adap = " (adaptation field only)"
        elif adap_field_ctrl == 0x3:
            s_adap = " (payload + adaptation field)"
        else:
            s_adap = " (Bad adaptation field control)"

    else:
        s_pid = "(TS packet)"
        indx_offs     = 0
        odd_even        = (ts_pkt[3] & 0x40) >> 6   # bit 7
        scrambling_en   = (ts_pkt[3] & 0x80) >> 7   # bit 8
        adap_field_ctrl = (ts_pkt[3] & 0x30) >> 4   # bits 5 and 4
        # 1 = payload only
        # 2 = adaptation field only
        # 3 = both
        # 0 = reserved (shouldn't happen)
        if adap_field_ctrl == 0x1:
            s_adap = " (payload only)"
            payload_start = 4;
        elif adap_field_ctrl == 0x2:
            s_adap = " (adaptation field only)"
            payload_start = 188
        elif adap_field_ctrl == 0x3:
            s_adap = " (payload + adaptation field)"
            payload_start = ts_pkt[4] + 4 + 1;
        else:
            s_adap = " (BAD adaptation field control)"
            payload_start = 4;

    pid = ((ts_pkt[indx_offs + 1] << 8) | ts_pkt[indx_offs + 2]) & 0x1fff

    print("")
    print("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~")
    print("PID: " + "0x{:03X}  ".format(pid) + s_pid , end='\n')
    print("Adapt_Field: " + "0x{:02X}  ".format(adap_field_ctrl) + s_adap, end='\n')
    print("Payload Start: " + "0x{:02X}".format(payload_start)  , end='\n')
    print("Scrambled: " + str(scrambling_en), end='\n')
    if scrambling_en == 1:
        print("Odd/Even: " + str(odd_even), end='\n')
        if is_fpga_format == True:
            print("Key: " + "0x{0:016X}".format(key), end='\n')

    print("")
    print(" b0    b1    b2    b3    b4    b5    b6    b7")
    for i in range(len(ts_pkt)):
        if ((i%8) == 7):
            print("0x{:02X}".format(ts_pkt[i]) + "   #" + str(i//8)  , end='\n')
        else:
            print("0x{:02X}".format(ts_pkt[i]), end='  ')



#   _____                  _             _       _____       __   ____
#  / ____|                | |           | |     |_   _|     / /  / __ \
# | (___     ___     ___  | | __   ___  | |_      | |      / /  | |  | |
#  \___ \   / _ \   / __| | |/ /  / _ \ | __|     | |     / /   | |  | |
#  ____) | | (_) | | (__  |   <  |  __/ | |_     _| |_   / /    | |__| |
# |_____/   \___/   \___| |_|\_\  \___|  \__|   |_____| /_/      \____/


class InputSocket(mp.Process):
    "this time with asyn recving"
    def __init__(self, mcast, port, interface):
        mp.Process.__init__(self, name="InputSocket_Process")
        self.daemon = True
        self.stop_prcs = mp.Event()
        self.stop_prgm = mp.Event()

        self.data_q = mp.Queue(QUEUE_SIZE)

        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.setblocking(1)  # blocking socket
            self.sock.settimeout(3.0)
            self.sock.bind(('', port))
            mreq = struct.pack("=4s4s", socket.inet_aton(mcast), socket.inet_aton(interface))
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            logging.info("\t%s: "+G_CLR+"connected @%s (%d) "+RST_CLR+"using %s NIC", self.__class__.__name__, mcast, port, interface)
        except IOError as e:
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
            logging.info("\tExit. Bye...")
            sys.exit()
        except:
            print ("Unexpected error:", sys.exc_info()[0])
            raise


    def run(self):
        cnt         = 0
        nbytes      = 0
        nbytes_lost = 0
        start = timer()
        signal.signal(signal.SIGINT, signal.SIG_IGN) # disable the capture of the keyboard interrupt signal (CTRL+C)

        while not self.stop_prcs.is_set():

            try:
                if cnt%NR_PKT_TEST_BPS == 0:
                    start = timer()
                    nbytes = 0
                    cnt = 0
                cnt +=1

                #receive chunks from the socket
                chunk = self.sock.recv(DATA_CHUNK_IN_BYTES)

                nbytes += len(chunk)
                #print("cnt=" + str(cnt) + "  len(chunk)=" + str(len(chunk)))
                if cnt%NR_PKT_TEST_BPS == (NR_PKT_TEST_BPS-1):
                    delta = timer() - start
                    bps =  (8*nbytes)/delta
                    if nbytes_lost != 0:
                        logging.warning("I_SOCK Mbps=%3.2f\tsource data queue FULL; "+R_CLR+"Lost %4d kb "+RST_CLR+"of data", bps/1e6, nbytes_lost/1e3)
                    else:
                        logging.info("\tI_SOCK Mbps=%3.2f", bps/1e6)
                    nbytes_lost = 0

                #send chunks to the queue
                #self.data_q.put(chunk, True, 3)
                self.data_q.put(chunk, False)
                #print("after cnt=" + str(cnt) + "  len(chunk)=" + str(len(chunk)))

            except socket.timeout:
                logging.warning("%s:\t\t"+Y_CLR+"no input data ..."+RST_CLR, self.__class__.__name__)
                continue
            except queue.Full:
                nbytes_lost += len(chunk)
                continue
            except IOError as e:
                logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
                continue



    def close_me(self):

        self.terminate()

        try:
            #self.sock.shutdown(socket.SHUT_RD)
            self.sock.close()
            logging.info("\t%s closed", self.__class__.__name__)
        except IOError as e:
            logging.warning("%s: Problem closing the socket", self.__class__.__name__)
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)


        try:
            self.data_q.close()
            self.data_q.cancel_join_thread()
        except IOError as e:
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)


        logging.info("\t%s stopped.", self.name)





class OutputSocket(mp.Process):
    "this time with asyn recving"
    def __init__(self, addr, port):
        mp.Process.__init__(self, name="OutputSocket_Process")
        self.daemon = True
        self.stop_prcs = mp.Event()
        self.stop_prgm = mp.Event()
        self.data_q = mp.Queue(QUEUE_SIZE)


        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.settimeout(3.0)
            self.sock.connect((addr, port))
            logging.info("\t%s: "+G_CLR+"connected @%s (%d)"+RST_CLR, self.__class__.__name__, addr, port)
        except IOError as e:
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
            logging.info("\tExit. Bye...")
            sys.exit()
        except:
            print ("Unexpected error:", sys.exc_info()[0])
            raise


    def run(self):
        cnt = 0
        nbytes = 0
        start = timer()
        signal.signal(signal.SIGINT, signal.SIG_IGN) # disable the capture of the keyboard interrupt signal (CTRL+C)


        while not self.stop_prcs.is_set():
            try:
                #receive chunks from the source queue
                chunk = self.data_q.get(True, 3)


                if cnt%NR_PKT_TEST_BPS == 0:
                    start = timer()
                    nbytes = 0
                    cnt = 0

                #send chunks to the socket
                sent = self.sock.send(chunk)

                nbytes += len(chunk)

                if cnt%NR_PKT_TEST_BPS == (NR_PKT_TEST_BPS-1):
                    delta = timer() - start
                    bps =  (8*nbytes)/delta
                    logging.info("\t"+D_CLR+"O_SOCK Mbps=%3.2f"+RST_CLR, bps/1e6)

                cnt +=1

                if chunk[sent:]:    # left-overs
                    logging.warning("%s: "+RST_CLR+"Data loss during transmition"+RST_CLR, self.__class__.__name__)


            except queue.Empty:
                logging.warning("%s\t\tsink data_queue EMPTY", self.__class__.__name__)
                continue
            except IOError as e:
                logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
                self.stop_prgm.set()



    def close_me(self):

        self.terminate()

        try:
            self.sock.shutdown(socket.SHUT_WR)
            self.sock.close()
            logging.info("\t%s closed", self.__class__.__name__)
        except IOError as e:
            logging.warning("%s: Problem closing the socket", self.__class__.__name__)
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)


        logging.info("\t%s stopped.", self.name)



#  ______   _   _            _____       __   ____
# |  ____| (_) | |          |_   _|     / /  / __ \
# | |__     _  | |   ___      | |      / /  | |  | |
# |  __|   | | | |  / _ \     | |     / /   | |  | |
# | |      | | | | |  __/    _| |_   / /    | |__| |
# |_|      |_| |_|  \___|   |_____| /_/      \____/

class FileReader(mp.Process):

    def __init__(self, filename=""):
        mp.Process.__init__(self, name="FileReader_Process")
        self.daemon = True
        self.stop_prcs = mp.Event()
        self.stop_prgm = mp.Event()
        self.data_q = mp.Queue(QUEUE_SIZE)
        self.filename = filename
        try:
            self.filehandle = open(self.filename, 'rb')
            logging.info("\tRead from File -> "+G_CLR+"Opened \'%s\' "+RST_CLR+"input file", self.filename)
        except IOError as e:
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
            logging.info("    Exit. Bye...")
            sys.exit()


    def run(self):
        cnt = 0
        nbytes = 0
        nbytes_lost = 0
        signal.signal(signal.SIGINT, signal.SIG_IGN) # disable the capture of the keyboard interrupt signal (CTRL+C)


        while not self.stop_prcs.is_set():
            try:

                # Calc performance
                if cnt%NR_PKT_TEST_BPS == 0:
                    start = timer()
                    nbytes = 0
                    cnt = 0


                #receive chunks from the file
                chunk = self.filehandle.read(DATA_CHUNK_IN_BYTES)

                #print(chunk)
                nbytes += len(chunk)

                if cnt%NR_PKT_TEST_BPS == (NR_PKT_TEST_BPS-1):
                    delta = timer() - start
                    bps =  (8*nbytes)/delta
                    if nbytes_lost != 0:
                        logging.info("\tI_File Mbps=%3.2f\tsource data queue FULL; "+R_CLR+"Lost %4d kb "+RST_CLR+"of data", bps/1e6, nbytes_lost/1e3)
                    else:
                        logging.info("\tI_File Mbps=%3.2f", bps/1e6)
                    nbytes_lost = 0
                cnt +=1

                if len(chunk) == 0:
                    raise EOFError

                #send chunks to the queue
                self.data_q.put(chunk, True, 3)

            except queue.Full:
                nbytes_lost += len(chunk)
                continue
            except EOFError:
                logging.info("\t%s:\t\t"+Y_CLR+"reached EOF of \'%s\'"+RST_CLR, self.__class__.__name__, self.filename)
                self.stop_prcs.set()
                continue


        self.filehandle.close()
        logging.info("\t%s:\t\tInput file \'%s\' closed.",  self.__class__.__name__, self.filename)



    def close_me(self):

        self.terminate()

        try:
            self.filehandle.close()
            logging.info("\tInput file \'%s\' closed.", self.filename)
        except:
            pass

        logging.info("\t%s stopped.", self.name)



class FileWriter(mp.Process):

    def __init__(self, filename="default_csav3_out_file"):
        mp.Process.__init__(self, name="FileWriter_Process")
        self.daemon = True
        self.data_q = mp.Queue(QUEUE_SIZE)
        self.stop_prcs = mp.Event()
        self.stop_prgm = mp.Event()
        self.filename   = filename
        try:
            self.filehandle = open(self.filename, 'wb')
            logging.info("\tWrite to File  -> "+G_CLR+"Created \'%s\'"+RST_CLR+" output file", filename)
        except IOError as e:
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
            logging.info("\tExit. Bye...")
            sys.exit()

    def run(self):
        cnt = 0
        nbytes = 0
        start = timer()
        signal.signal(signal.SIGINT, signal.SIG_IGN) # disable the capture of the keyboard interrupt signal (CTRL+C)

        while not self.stop_prcs.is_set():
            try:

                #receive chunks from the elab queue
                chunk = self.data_q.get(True, 3)
                # print(chunk)


                if cnt%NR_PKT_TEST_BPS == 0:
                    start = timer()
                    nbytes = 0
                    cnt = 0

                #send chunks to the file
                self.filehandle.write(chunk)

                nbytes += len(chunk)

                if cnt%NR_PKT_TEST_BPS == (NR_PKT_TEST_BPS-1):
                    delta = timer() - start
                    bps =  (8*nbytes)/delta
                    logging.info("\t"+D_CLR+"O_File Mbps=%3.2f"+RST_CLR, bps/1e6)

                cnt +=1

            except queue.Empty:
                self.filehandle.flush()
                logging.warning("%s:\t\tsink data_queue EMPTY", self.__class__.__name__)
                continue
            except IOError as e:
                logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)
                self.stop_prcs.set()
                continue

        print("closing file")
        self.filehandle.flush()
        self.filehandle.close()
        logging.info("\tOutput file \'%s\' closed.", self.filename)






    def close_me(self):

        self.terminate()

        try:
            self.filehandle.flush()
            self.filehandle.close()
            logging.info("\tOutput file \'%s\' closed.", self.filename)
        except:
            pass


        logging.info("\t%s stopped.", self.name)





#  ______   _           _                              _
# |  ____| | |         | |                            | |
# | |__    | |   __ _  | |__     ___    _ __    __ _  | |_    ___
# |  __|   | |  / _` | | '_ \   / _ \  | '__|  / _` | | __|  / _ \
# | |____  | | | (_| | | |_) | | (_) | | |    | (_| | | |_  |  __/
# |______| |_|  \__,_| |_.__/   \___/  |_|     \__,_|  \__|  \___|

class Elaborate(mp.Process):
    def __init__(self, video_pid, ecm_pid):
        mp.Process.__init__(self, name="Elaborate_Process")
        self.daemon = True
        self.ecm_pid    = ecm_pid
        self.video_pid  = video_pid
        self.stop_prcs = mp.Event()
        self.stop_prgm = mp.Event()

    def set_data_queue(self, source_data_q, sink_data_q):
        self.source_data_q = source_data_q
        self.sink_data_q = sink_data_q



    def extract_key(self, key_list, ts_pkt):
        json_data = ts_pkt[7:].decode('utf-8', 'ignore')
        json_obj  = json.loads(json_data)
        cp = json_obj["CP_CW_List"][0]["CP"]
        if(cp & 0x1):
            key_list[1] = json_obj["CP_CW_List"][0]["CW"]
        else:
            key_list[0] = json_obj["CP_CW_List"][0]["CW"]

        cp = json_obj["CP_CW_List"][1]["CP"]
        if(cp & 0x1):
            key_list[1] = json_obj["CP_CW_List"][1]["CW"]
        else:
            key_list[0] = json_obj["CP_CW_List"][1]["CW"]



    def create_ts_pkt_fpga_format(self, key_list, ts_pkt):
        adap_field_ctrl = (ts_pkt[3] & 0x30) >> 4   # bits 5 and 4
        odd_even        = (ts_pkt[3] & 0x40) >> 6   # bit 7
        scrambling_en   = (ts_pkt[3] & 0x80) >> 7   # bit 8

        # 1 = payload only
        # 2 = adaptation field only
        # 3 = both
        # 0 = reserved (shouldn't happen)
        if adap_field_ctrl == 0x1:
            payload_start = 4;
        elif adap_field_ctrl == 0x2:
            payload_start = 188
        elif adap_field_ctrl == 0x3:
            payload_start = ts_pkt[4] + 4 + 1;
        else:
            #std::cerr << "Bad adaptation field control" << std::endl;
            payload_start = 4;

        header = bytearray([0]*8)                       # 64 bit key (MSBs of 128 bit key)

        for bit_shift in reversed(range(0, 64, 8)):     # 64 bit key (LSBs of 128 bit key)
            key = int(key_list[odd_even], 16)
            header.append( (key>>bit_shift) & 0xff )

        header = header + bytearray([0]*6)              # reserved all '0'
        header.append(payload_start)                    # payload start
        header.append((odd_even<<1) + scrambling_en)
        header = header + bytearray([0]*8)

        return (header + ts_pkt)    # 32 + 188 = 220 bytes




    def run(self):
        logging.info("\tStart elaborating ...")
        cnt = 0
        total_pkt_count = 0 # doesn't reset
        nr_ts_packets = 0
        odd_even = 0
        odd_even_d = 0
        formated_pkts = []
        key_list = ['0x0000000000000000','0x0000000000000000']
        cc_i     = 0
        cc_i_d   = 0
        cc_cnt_i = 0
        cc_e     = 0
        cc_e_d   = 0
        cc_cnt_e = 0
        cc_cnt_i_old = 0
        cc_cnt_e_old = 0
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        while not self.stop_prcs.is_set():
            #chunk_out = bytearray()
            chunk_out = bytes()

            try:


                chunk_in = self.source_data_q.get(True, 3)
                #print(chunk_in)


                if not BYPASS:

                    # ELABORATE ........
                    # there might be an RTP header so strip that off
                    chunk_in = chunk_in[chunk_in.index(0x47):]
                    # break into TS packets
                    ts_pkts = [chunk_in[i:i+188] for i in range(0, len(chunk_in), 188)]
                    #ts_pkt is a bytearray
                    for ts_pkt in ts_pkts:
                        if ts_pkt[0] != 0x47:
                            logging.warning("%s:\t "+RST_CLR+"TS packet synch lost..input chunk of data thrown away", self.__class__.__name__)
                            break

                        pid = ((ts_pkt[1] << 8) | ts_pkt[2]) & 0x1fff

                        # Extract encryption key from ECM packet
                        if pid == self.ecm_pid :
                            self.extract_key(key_list, ts_pkt)

                            #print ("key0:key1i ", key_list[0], " ", key_list[1])
                            #print_ts(ts_pkt)
                            continue

                        # Display key changing
                        if DEBUG:
                            if pid == self.video_pid:
                                odd_even_d = odd_even
                                odd_even        = (ts_pkt[3] & 0x40) >> 6   # bit 7
                                if odd_even_d != odd_even:
                                   logging.info("\tkey change  ~> \t  key0=%s; key1=%s", key_list[0], key_list[1])
                                   #print_ts(ts_pkt)
                                cc_i_d = cc_i
                                cc_i   = ts_pkt[3] & 0xf
                                if cc_i != (cc_i_d + 1) % 16:
                                    cc_cnt_i +=1


                        # ignore null packets
#                        if pid != 0x1fff:
                        if True:
                        # Create FPGA format TS packet
                            ts_pkt_fpga_format = self.create_ts_pkt_fpga_format(key_list, ts_pkt)
                            print_ts(ts_pkt_fpga_format)
                        else:
                            continue


                        ######################################
                        #          Call PCIe drivers         #
                        ######################################
                        if DO_DECRYPTION:
                            pcie.de_crypt(ts_pkt_fpga_format)



                        if DEBUG:
                            if pid == self.video_pid:
                                cc_e_d = cc_e
                                cc_e   = ts_pkt_fpga_format[35] & 0xf
                                if cc_e != (cc_e_d +1) % 16:
                                    cc_cnt_e +=1


                        chunk_out += ts_pkt_fpga_format[32:]
                        #print_ts(chunk_out)

                else:
                    # send the raw data to the card without any processing.
                    # This only really makes sense if the file we're processing is a
                    # binary file containing the correct four word header to hold
                    # the keys.
                    if DO_DECRYPTION:
                        pcie.de_crypt(chunk_in)

                    chunk_out = chunk_in

                    # Hack to fix problem with one byte getting a non-zero value
                    # which stops exact comparison with the expected data.
#                        if READ_FROM_FILE:
#                            chunk_out[220] = 0

                # Calc performance
                if cnt%NR_PKT_TEST_BPS == 0:
                    start = timer()
                    nbytes = 0
                    cnt = 0



                self.sink_data_q.put(chunk_out, True, 3)


                nbytes += len(chunk_in)
                nr_ts_packets += len(chunk_in)/188


                if cnt%NR_PKT_TEST_BPS == (NR_PKT_TEST_BPS-1):
                    delta = timer() - start
                    bps =  (8*nbytes)/delta

                    if cc_cnt_i != cc_cnt_i_old:
                        strr1 = R_CLR+"%d"+RST_CLR
                    else:
                        strr1 = "%d"

                    if cc_cnt_e != cc_cnt_e_old:
                        strr2 = R_CLR+"%d"+RST_CLR
                    else:
                        strr2 = "%d"

                    cc_cnt_i_old = cc_cnt_i
                    cc_cnt_e_old = cc_cnt_e


                    if BYPASS:
                        logging.info("\tElab   Mbps=%3.2f\tTS pkt elab %3.2fM ", bps/1e6, nr_ts_packets/1e6)
                    else:
                        logging.info("\tElab   Mbps=%3.2f\tTS pkt elab %3.2fM  CC_src="+strr1+";  CC_decrypt="+strr2, bps/1e6, nr_ts_packets/1e6, cc_cnt_i, cc_cnt_e)

                cnt +=1
                total_pkt_count += 1
#                    if total_pkt_count==10000:
#                        print("attempting to stop")
#                        sys.exit(9)

            except queue.Full:
                logging.error("\t%s:\t\tsink data queue FULL; "+R_CLR+"Lost chunk of elaborated data"+RST_CLR, self.__class__.__name__)
                continue
            except queue.Empty:
                logging.warning("%s\t\tsource data_queue EMPTY", self.__class__.__name__)
                continue
            except IOError as e:
                logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)


    def close_me(self):

        self.terminate()

        try:
            self.sink_data_q.close()
            self.sink_data_q.cancel_join_thread()
        except IOError as e:
            logging.error("\t%s: I/O error({0}): {1}".format(e.errno, e.strerror), self.__class__.__name__)

        logging.info("\t%s stopped.", self.name)





def close_program(source, elab, sink, pcie_conn):
    print("")
    logging.info("\t"+Y_CLR+"Closing..."+RST_CLR)

    source.stop_prcs.set()
    elab.stop_prcs.set()
    sink.stop_prcs.set()
    time.sleep(1.0)

    sink.close_me()
    elab.close_me()
    source.close_me()

    if ENABLE_PCIE:
        if (pcie.disconnect() != 0):
            logging.error("\t"+R_CLR+"Could not close PCIe connection"+RST_CLR)
        else:
            if pcie_conn == True:
                logging.error("\tPCIe card disconected")

    #try:
    sink.join()
    source.join()
    elab.join()
    #except AssertionError:
    #    pass

    logging.info("\tExit. Bye...")
    sys.exit()


#  __  __           _
# |  \/  |         (_)
# | \  / |   __ _   _   _ __
# | |\/| |  / _` | | | | '_ \
# | |  | | | (_| | | | | | | |
# |_|  |_|  \__,_| |_| |_| |_|

if __name__ == '__main__':


    #logging.basicConfig(filename='csav3_streamer.log', filemode='w', level=logging.DEBUG, format='%(asctime)s %(levelname)s: %(message)s', datefmt='%m/%d/%Y %I:%M:%S %p')
    logging.basicConfig(format='%(asctime)s %(levelname)s: %(message)s', datefmt='%m/%d/%Y %I:%M:%S %p', level=logging.DEBUG)
    #logging.basicConfig(format='%(levelname)s: %(message)s', level=logging.DEBUG)


    print("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~")
    logging.info('\tStarting CSAv3 Streamer script')




    READ_FROM_FILE = 0      # 0 = read from SOCKET
                            # 1 = read from FILE
    WRITE_TO_FILE  = 0      # 0 = write to SOCKET
                            # 1 = write to FILE


    VIDEO_PID   = 50     # Video pid to be descrambled
    ECM_PID     = 52     # entitlement control message

    #r_filename = "TSDecryptCapture.ts"
    #r_filename = "CSA3_ROLLING_CW_VID_50_ECM_52_POS_DELAY_START.ts"
    #r_filename = "UUTScrambledOTSCapture_0.ts"
    r_filename = "text_document.txt"

    w_filename = "csa3_rolling_out.ts"
    #w_filename = "out.txt"

    # Create input thread
    if (READ_FROM_FILE == 1):
        source = FileReader(r_filename)
    else:
        # Roger's encrypted multicast
        #MCST = '239.1.59.68'
        #PORT = 5000

        # BBC feed
#        MCST = '239.100.1.1'
        #PORT = 5000

        # File stream (rolling key's from Eltion's PC)
        MCST = '239.100.1.1'
        PORT = 5030

        #MCST = '127.0.0.1'
        #PORT = 5000
        IFCE = '192.168.58.129'
        source = InputSocket(MCST, PORT, IFCE)



    # Create output thread
    if (WRITE_TO_FILE == 1):
        sink = FileWriter(w_filename)
    else:
        # WHOST = '127.0.0.1'
        WHOST = '239.100.1.1'
        # WHOST = '239.1.59.68'
        WPORT = 5010
        sink = OutputSocket(WHOST, WPORT)


    # Create output thread
    elab = Elaborate(VIDEO_PID, ECM_PID)

    elab.set_data_queue(source.data_q, sink.data_q)

    pcie_conn = False

    try:

        if BYPASS == True:
            logging.warning(""+Y_CLR+"Bypass Enabled"+RST_CLR)

        if DO_DECRYPTION == False:
            logging.warning(""+Y_CLR+"Decryption is Disabled"+RST_CLR)



        if ENABLE_PCIE:
            if (pcie.connect() != 0):
                logging.error("\t"+R_CLR+"Could not connect to PCIe"+RST_CLR)
                pcie_conn = False
                raise KeyboardInterrupt
            else:
                logging.info("\t"+G_CLR+"Connected succesfully to PCIe"+RST_CLR)
                pcie_conn = True
        else:
            logging.info("\t"+ Y_CLR + "PCIe is disabled" + RST_CLR)


        source.start()
        elab.start()
        sink.start()

        while (1):
            time.sleep(0.05)
            if source.stop_prgm.is_set() == True or elab.stop_prgm.is_set() == True or sink.stop_prgm.is_set() == True:
                raise KeyboardInterrupt

    except KeyboardInterrupt:
        close_program(source, elab, sink, pcie_conn)


