#include "array.c"
#include <net/if.h>
#include <sys/ioctl.h>

#define BUFFSIZE 256
#define UPLIMIT 254
#define LOWLIMIT 2

int isON = 1; //status receiver ON
/*socket*/
int sock; //socket
struct ifreq ifr; //gatau, dapet dari internet
struct sockaddr_in myaddr; //address program ini sebaga receiver
struct sockaddr_in trnsmtaddr; //address transmitter
socklen_t myaddrlen = sizeof(myaddr); //ukuran socket ini
/*buffer*/
char queue[BUFFSIZE]; //antrian karakter sirkular
int headQueue = 0; //antrian pertama siap dikonsum
int tailQueue = 0; //space kosong setelah antrian terakhir
/*array*/
Array receivedBytes;
ArrayFrame receivedFrames;
int processedIncomingBytes;

unsigned char countChecksum(unsigned char* ptr, size_t sz) {
    unsigned char chk = 0;
    while (sz-- != 0) {
        chk -= *ptr++;
    }
    /*debug*/printf("[COUNTCHECKSUM] %x\n", chk);
    return chk;
}

unsigned char getByte() {
    while(processedIncomingBytes >= receivedBytes.used); //wait until new incoming byte
    unsigned char result = receivedBytes.data[processedIncomingBytes++];
    /*debug*/printf("[GETBYTE] %x\n", result);
    return result;
}

void sendByte(char buffX) {
    /*debug*/printf("[SENDBYTE] %x\n", buffX);
    if (sendto(sock, &buffX, 1, 0, (struct sockaddr*)& trnsmtaddr, myaddrlen) < 0) {
        printf("sendto() failed.\n");
        exit(1);
    }
}

void sendACK(int fnum) {
    //build ACK packet
    unsigned char ackpacket[6];
    ackpacket[0] = ACK;
    ackpacket[4] = (fnum >> 24) & 0xFF;
    ackpacket[3] = (fnum >> 16) & 0xFF;
    ackpacket[2] = (fnum >> 8) & 0xFF;
    ackpacket[1] = fnum & 0xFF;
    ackpacket[5] = countChecksum(ackpacket , 5);
    //send
    /*debug*/printf("[SENDACK]\n");
    int i = 0;
    while (i < 6) {
        sendByte(ackpacket[i++]);
    }
}

/* ===== ADT QUEUE ===== */
int isBufferEmpty() {
    return headQueue == tailQueue ? 1 : 0;
}

int isBufferLowLimit() {
    if (tailQueue >= headQueue) {
        return tailQueue - headQueue < LOWLIMIT ? 1 : 0;
    } else {
        return tailQueue + BUFFSIZE - headQueue < LOWLIMIT ? 1 : 0;
    }
}

int isBufferUpLimit() {
    if (tailQueue >= headQueue) {
        return tailQueue - headQueue > UPLIMIT ? 1 : 0;
    } else {
        return tailQueue + BUFFSIZE - headQueue > UPLIMIT ? 1 : 0;
    }
}

void addToBuffer(char c) {
    /*debug*/printf("[ADDTOBUFFER] %x\n", c);
    queue[tailQueue++] = c;
    tailQueue %= BUFFSIZE;
}

char delFromBuffer() {
    char result = queue[headQueue++];
    headQueue %= BUFFSIZE;
    /*debug*/printf("[DELFROMBUFFER] %x\n", result);
    return result;
}

/* ===== THREAD ===== */
void* byteHandler() {
    int lenSleep = 100000; //jeda saat mengkonsumsi data, nanosecond
    //loop selama antrian masih ada
    while (1) {
        while (isBufferEmpty() == 0) {
            unsigned char rcvchar = delFromBuffer();
            insertArray(&receivedBytes, rcvchar);
            /*debug*/printf("[BYTEHANDLER] %x\n", rcvchar);
            if (isBufferLowLimit() && isON == 0) { //transmitter bisa lanjutin
                printf("Buffer < lowerlimit\n");
                printf("Mengirim XON.\n");
                sendByte(XON);
                isON = 1;
            }
        }
    }
}

void* frameHandler() {
    FrameData thisFrame;
    Array tempFrame;
    Array tempText;
    initArray(&tempFrame, 5);
    initArray(&tempText, 5);
    //loop sampai paket habis
    while (1) {
        if (getByte() == SOH) {
            insertArray(&tempFrame, SOH);
            /*debug*/printf("[FRAMEIDENTIFIER] SOH\n");
            //ambil 4 byte lalu jadikan integer
            unsigned char frameID[4];
            frameID[0] = getByte();
            frameID[1] = getByte();
            frameID[2] = getByte();
            frameID[3] = getByte();
            int i;
            for (i = 0; i < 4; i++) {
                /*debug*/printf("[FRAMEIDENTIFIER] ID %d %x\n", i, frameID[i]);
            }
            insertArray(&tempFrame, frameID[0]);
            insertArray(&tempFrame, frameID[1]);
            insertArray(&tempFrame, frameID[2]);
            insertArray(&tempFrame, frameID[3]);
            int t_num = *((int*) frameID);
            /*debug*/printf("[FRAMEIDENTIFIER] ID %d\n", t_num);
            if (getByte() == STX ) {
                insertArray(&tempFrame, STX);
                /*debug*/printf("[FRAMEIDENTIFIER] STX\n");
                //ambil data
                int countLength = 0;
                unsigned char realText;
                do {
                    realText = getByte();
                    if (realText == ETX) {
                        insertArray(&tempFrame, ETX);
                        /*debug*/printf("[FRAMEIDENTIFIER] ETX\n");
                        if (getByte() == countChecksum(tempFrame.data, tempFrame.used)) {
                            thisFrame.id = t_num;
                            thisFrame.length = countLength;
                            int i;
                            /*debug*/printf("[FRAMEIDENTIFIER] frame received: ");
                            for (i = 0; i < tempText.used; i++) {
                                printf("%c", tempText.data[i]);
                                thisFrame.text[i] = tempText.data[i];
                            }
                            printf("\n");
                            insertArrayFrame(&receivedFrames, thisFrame);
                            sendACK(t_num);
                        }
                        break;
                    }
                    //bukan ETX
                    insertArray(&tempFrame, realText);
                    insertArray(&tempText, realText);
                    /*debug*/printf("[FRAMEIDENTIFIER] DATA %x\n", realText);
                } while(countLength < FRAMEDATASIZE);
            }
        }
        freeArray(&tempText);
        freeArray(&tempFrame);
    }
}

/* ===== MAIN PROGRAM ===== */
int main(int argc, char* argv[]) {
    if (argc == 2) {
        //inisialisasi
        initArray(&receivedBytes, 5);
        initArrayFrame(&receivedFrames,5);
        processedIncomingBytes = 0;
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == -1) {
            printf("socket() failed.\n");
            exit(1);
        }
        //susun alamat internet kita
        memset((char*)& myaddr, 0, myaddrlen); //clear memory jadi nol
        myaddr.sin_family = AF_INET; //setting IPv4
        myaddr.sin_addr.s_addr = htonl(INADDR_ANY); //IP address local
        myaddr.sin_port = htons(atoi(argv[1]));
        //detect ip wireless, http://stackoverflow.com/questions/2283494/get-ip-address-of-an-interface-on-linux
        ifr.ifr_addr.sa_family = AF_INET;
        strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
        ioctl(sock, SIOCGIFADDR, &ifr);
        printf("Binding pada %s:%s ...\n", inet_ntoa(((struct sockaddr_in*)& ifr.ifr_addr) -> sin_addr), argv[1]);
        if(bind(sock, (struct sockaddr*)& myaddr, myaddrlen) < 0) {
            printf("bind() failed.\n");
            exit(1);
        }
        //receiving
        pthread_t tByteHandler;
        pthread_t tFrameHandler;
        int countByte = 0; //counter karakter
        char rcvchar; //tempat menampung karakter yang diterima
        //loop sampai break di ENDFILE
        while (1) {
            //menerima 1 karakter
            if (recvfrom(sock, &rcvchar, 1, 0, (struct sockaddr*)& trnsmtaddr, &myaddrlen) > 0) {
                addToBuffer(rcvchar);
                if (rcvchar == STARTFILE) {
                    pthread_create(&tByteHandler, NULL, byteHandler, "byteHandler"); //jalankan thread 
                    pthread_create(&tFrameHandler, NULL, frameHandler, "frameHandler"); //jalankan thread
                    printf("Thread started!\n");
                } else if (rcvchar == ENDFILE) {
                    pthread_join(tByteHandler, NULL);
                    pthread_join(tFrameHandler, NULL);
                    printf("Thread finished!\n");
                    break;
                } else {
                    //printf("Menerima byte ke-%d : %x.\n", ++countByte , rcvchar);
                    if (isBufferUpLimit()) { //transmit menunda pengiriman
                        printf("Buffer > minimum upperlimit\n");
                        printf("Mengirim XOFF.\n");
                        sendByte(XOFF);
                        isON = 0;
                    }
                }
            }
        }
        close(sock);
    } else { //arg invalid
        printf("Usage : ./receiver port\n");
        exit(1);
    }
    return 0;
}
