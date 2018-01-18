

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
    logging.info('\tStarting Tx script')




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
    IFCE = '10.43.2.124'
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




