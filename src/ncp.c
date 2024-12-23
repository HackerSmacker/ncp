/* Daemon implementing the ARPANET NCP.    Talks to the IMP interface
     and applications. */

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "imp.h"
#include "wire.h"

#define IMP_REGULAR             0
#define IMP_LEADER_ERROR    1
#define IMP_DOWN                    2
#define IMP_BLOCKED             3
#define IMP_NOP                     4
#define IMP_RFNM                    5
#define IMP_FULL                    6
#define IMP_DEAD                    7
#define IMP_DATA_ERROR        8
#define IMP_INCOMPL             9
#define IMP_RESET                10

#define LINK_CTL         0
#define LINK_MIN         2
#define LINK_MAX        71
#define LINK_ECHO     72
#define LINK_IP        155

#define NCP_NOP            0
#define NCP_RTS            1
#define NCP_STR            2
#define NCP_CLS            3
#define NCP_ALL            4
#define NCP_GVB            5
#define NCP_RET            6
#define NCP_INR            7
#define NCP_INS            8
#define NCP_ECO            9
#define NCP_ERP         10
#define NCP_ERR         11
#define NCP_RST         12
#define NCP_RRP         13
#define NCP_MAX         NCP_RRP

#define ERR_UNDEFINED     0 //Undefined.
#define ERR_OPCODE            1 //Illegal opcode.
#define ERR_SHORT             2 //Short parameter space.
#define ERR_PARAM             3 //Bad parameters.
#define ERR_SOCKET            4 //Request on a non-existent socket.
#define ERR_CONNECT         5 //Socket(link) not connected.

#define CONNECTIONS 20

static int fd;
static struct sockaddr_un server;
static struct sockaddr_un client;
static socklen_t len;

struct {
    struct sockaddr_un client;
    socklen_t len;
    int host;
    struct { int link, size; uint32_t lsock, rsock; } rcv, snd;
} connection[CONNECTIONS];

struct {
    struct sockaddr_un client;
    socklen_t len;
    uint32_t sock;
} listening[CONNECTIONS];

static const char *type_name[] = {
    "NOP", // 0
    "RTS", // 1
    "STR", // 2
    "CLS", // 3
    "ALL", // 4
    "GVB", // 5
    "RET", // 6
    "INR", // 7
    "INS", // 8
    "ECO", // 9
    "ERP", // 10
    "ERR", // 11
    "RST", // 12
    "RRP"    // 13
};

static uint8_t packet[200];
static uint8_t app[200];

static int find_link(int host, int link) {
    int i;
    for(i = 0; i < CONNECTIONS; i++) {
        if(connection[i].host == host && connection[i].rcv.link == link)
            return i;
        if(connection[i].host == host && connection[i].snd.link == link)
            return i;
    }
    return -1;
}

static int find_socket(int host, uint32_t lsock) {
    int i;
    for(i = 0; i < CONNECTIONS; i++) {
        if(connection[i].host == host && connection[i].rcv.lsock == lsock)
            return i;
        if(connection[i].host == host && connection[i].snd.lsock == lsock)
            return i;
    }
    return -1;
}

static int find_sockets(int host, uint32_t lsock, uint32_t rsock) {
    int i;
    for(i = 0; i < CONNECTIONS; i++) {
        if(connection[i].host == host && connection[i].rcv.lsock == lsock
                && connection[i].rcv.rsock == rsock)
            return i;
        if(connection[i].host == host && connection[i].snd.lsock == lsock
                && connection[i].snd.rsock == rsock)
            return i;
    }
    return -1;
}

static int find_listen(uint32_t socket) {
    int i;
    for(i = 0; i < CONNECTIONS; i++) {
        if(listening[i].sock == socket)
            return i;
        if(listening[i].sock + 1 == socket)
            return i;
    }
    return -1;
}


static void destroy(int i) {
    connection[i].host = connection[i].rcv.link = connection[i].snd.link =
        connection[i].snd.size = connection[i].rcv.size = -1;
    connection[i].rcv.lsock = connection[i].rcv.rsock =
        connection[i].snd.lsock = connection[i].snd.rsock = 0;
}

static void send_imp(int flags, int type, int destination, int link, int id,
                                            int subtype, void *data, int words) {
    packet[12] = flags << 4 | type;
    packet[13] = destination;
    packet[14] = link;
    packet[15] = id << 4 | subtype;
    if(data != NULL)
        memcpy(packet + 16, data, 2 *(words - 2));

#if 1
    {
        int i;
        for(i = 12; i < 12 + 2*words; i += 2)
            fprintf(stderr, " <<< %06o(%03o %03o)\n",
                            (packet[i] << 8) | packet[i+1], packet[i], packet[i+1]);
    }
#endif

    imp_send_message(packet, words);
}

static void send_leader_error(int subtype) {
    send_imp(0, IMP_LEADER_ERROR, 0, 0, 0, subtype, NULL, 2);
}

static void send_nop(void) {
    send_imp(0, IMP_NOP, 0, 0, 0, 0, NULL, 2);
}

static void send_reset(void) {
    send_imp(0, IMP_RESET, 0, 0, 0, 0, NULL, 2);
}

static void send_ncp(uint8_t destination, uint8_t byte, uint16_t count,
                                            uint8_t type) {
    packet[16] = 0;
    packet[17] = byte;
    packet[18] = count >> 8;
    packet[19] = count;
    packet[20] = 0;
    packet[21] = type;
    fprintf(stderr, "NCP: send to %03o, type %d/%s.\n",
                     destination, type, type <= NCP_MAX ? type_name[type] : "???");
    send_imp(0, IMP_REGULAR, destination, 0, 0, 0, NULL,(count + 9 + 1)/2);
}

static int make_open(int host,
                                            uint32_t rcv_lsock, uint32_t rcv_rsock,
                                            uint32_t snd_lsock, uint32_t snd_rsock) {
    int i = find_link(-1, -1);
    if(i == -1) {
        fprintf(stderr, "NCP: Table full.\n");
        return -1;
    }

    connection[i].host = host;
    connection[i].rcv.lsock = rcv_lsock;
    connection[i].rcv.rsock = rcv_rsock;
    connection[i].snd.lsock = snd_lsock;
    connection[i].snd.rsock = snd_rsock;

    return i;
}

// Sender to receiver.
void ncp_str(uint8_t destination, uint32_t lsock, uint32_t rsock, uint8_t size) {
    packet[22] = lsock >> 24;
    packet[23] = lsock >> 16;
    packet[24] = lsock >> 8;
    packet[25] = lsock;
    packet[26] = rsock >> 24;
    packet[27] = rsock >> 16;
    packet[28] = rsock >> 8;
    packet[29] = rsock;
    packet[30] = size;
    send_ncp(destination, 8, 10, NCP_STR);
}

// Receiver to sender.
void ncp_rts(uint8_t destination, uint32_t lsock, uint32_t rsock, uint8_t link) {
    packet[22] = lsock >> 24;
    packet[23] = lsock >> 16;
    packet[24] = lsock >> 8;
    packet[25] = lsock;
    packet[26] = rsock >> 24;
    packet[27] = rsock >> 16;
    packet[28] = rsock >> 8;
    packet[29] = rsock;
    packet[30] = link;
    send_ncp(destination, 8, 10, NCP_RTS);
}

// Allocate.
void ncp_all(uint8_t destination, uint8_t link, uint16_t msg_space, uint32_t bit_space) {
    packet[22] = link;
    packet[23] = msg_space >> 16;
    packet[24] = msg_space;
    packet[25] = bit_space >> 24;
    packet[26] = bit_space >> 16;
    packet[27] = bit_space >> 8;
    packet[28] = bit_space;
    send_ncp(destination, 8, 8, NCP_ALL);
}

// Return.
void ncp_ret(uint8_t destination, uint8_t link, uint16_t msg_space, uint32_t bit_space) {
    packet[22] = link;
    packet[23] = msg_space >> 16;
    packet[24] = msg_space;
    packet[25] = bit_space >> 24;
    packet[26] = bit_space >> 16;
    packet[27] = bit_space >> 8;
    packet[28] = bit_space;
    send_ncp(destination, 8, 8, NCP_RET);
}

// Give back.
void ncp_gvb(uint8_t destination, uint8_t link, uint8_t fm, uint8_t fb) {
    packet[22] = link;
    packet[23] = fm;
    packet[24] = fb;
    send_ncp(destination, 8, 4, NCP_GVB);
}

// Interrupt by receiver.
void ncp_inr(uint8_t destination, uint8_t link) {
    packet[22] = link;
    send_ncp(destination, 8, 2, NCP_INR);
}

// Interrupt by sender.
void ncp_ins(uint8_t destination, uint8_t link) {
    packet[22] = link;
    send_ncp(destination, 8, 2, NCP_INS);
}

// Close.
void ncp_cls(uint8_t destination, uint32_t lsock, uint32_t rsock) {
    packet[22] = lsock >> 24;
    packet[23] = lsock >> 16;
    packet[24] = lsock >> 8;
    packet[25] = lsock;
    packet[26] = rsock >> 24;
    packet[27] = rsock >> 16;
    packet[28] = rsock >> 8;
    packet[29] = rsock;
    send_ncp(destination, 8, 9, NCP_CLS);
}

// Echo.
void ncp_eco(uint8_t destination, uint8_t data) {
    memset(packet, 0, sizeof packet);
    packet[22] = data;
    send_ncp(destination, 8, 2, NCP_ECO);
}

// Echo reply.
void ncp_erp(uint8_t destination, uint8_t data) {
    packet[22] = data;
    send_ncp(destination, 8, 2, NCP_ERP);
}

// Reset.
void ncp_rst(uint8_t destination) {
    send_ncp(destination, 8, 1, NCP_RST);
}

// Reset reply.
void ncp_rrp(uint8_t destination) {
    send_ncp(destination, 8, 1, NCP_RRP);
}

// No operation.
void ncp_nop(uint8_t destination) {
    send_ncp(destination, 8, 1, NCP_NOP);
}

// Error.
void ncp_err(uint8_t destination, uint8_t code, void *data, int length) {
    packet[22] = code;
    memcpy(packet + 23, data, length > 10 ? 10 : length);
    if(length < 10)
        memset(packet + 23 + length, 0, 10 - length);
    send_ncp(destination, 8, 12, NCP_ERR);
}

static int process_nop(uint8_t source, uint8_t *data) {
    return 0;
}

static uint32_t sock(uint8_t *data) {
    uint32_t x;
    x = data[0];
    x =(x << 8) | data[1];
    x =(x << 8) | data[2];
    x =(x << 8) | data[3];
    return x;
}

static void reply_open(uint8_t host, uint32_t socket, uint8_t connection) {
    uint8_t reply[7];
    reply[0] = WIRE_OPEN+1;
    reply[1] = host;
    reply[2] = socket >> 24;
    reply[3] = socket >> 16;
    reply[4] = socket >> 8;
    reply[5] = socket;
    reply[6] = connection;
    if(sendto(fd, reply, sizeof reply, 0,(struct sockaddr *)&client, len) == -1)
        fprintf(stderr, "NCP: sendto %s error: %s.\n",
                         client.sun_path, strerror(errno));
}

static void reply_listen(uint8_t host, uint32_t socket, uint8_t connection) {
    uint8_t reply[7];
    reply[0] = WIRE_LISTEN+1;
    reply[1] = host;
    reply[2] = socket >> 24;
    reply[3] = socket >> 16;
    reply[4] = socket >> 8;
    reply[5] = socket;
    reply[6] = connection;
    if(sendto(fd, reply, sizeof reply, 0,(struct sockaddr *)&client, len) == -1)
        fprintf(stderr, "NCP: sendto %s error: %s.\n",
                         client.sun_path, strerror(errno));
}

static void reply_close(uint8_t connection) {
    uint8_t reply[2];
    reply[0] = WIRE_CLOSE+1;
    reply[1] = connection;
    if(sendto(fd, reply, sizeof reply, 0,(struct sockaddr *)&client, len) == -1)
        fprintf(stderr, "NCP: sendto %s error: %s.\n",
                         client.sun_path, strerror(errno));
}

static int process_rts(uint8_t source, uint8_t *data) {
    int i;
    uint32_t lsock, rsock;
    rsock = sock(&data[0]);
    lsock = sock(&data[4]);

    fprintf(stderr, "NCP: Recieved RTS %u:%u from %03o.\n",
                     lsock, rsock, source);

    if(data[8] < LINK_MIN || data[8] > LINK_MAX) {
        ncp_err(source, ERR_PARAM, data - 1, 10);
        return 9;
    }

    if(find_listen(lsock) == -1) {
        i = find_sockets(source, lsock, rsock);
        if(i == -1) {
            fprintf(stderr, "NCP: Not listening to %u, no outgoing RFC, rejecting.\n", lsock);
            ncp_err(source, ERR_CONNECT, data - 1, 10);
            return 9;
        }
        fprintf(stderr, "NCP: Outgoing RFC socket %u.\n", lsock);
    } else {
        i = find_socket(source, lsock + 1);
        if(i == -1) {
            i = make_open(source, 0, 0, lsock, rsock);
            fprintf(stderr, "NCP: Listening to %u: new connection %d.\n", lsock, i);
        } else {
            connection[i].snd.lsock = lsock;
            connection[i].snd.rsock = rsock;
            fprintf(stderr, "NCP: Listening to %u: connection %d.\n", lsock, i);
        }
    }
    connection[i].snd.link = data[8]; //Send link.
    if(connection[i].rcv.size == -1) {
        connection[i].rcv.size = 8; //Send byte size.
        ncp_str(connection[i].host, lsock, rsock, connection[i].rcv.size);
        if(connection[i].rcv.link != -1) {
            fprintf(stderr, "NCP: Completing incoming RFC.\n");
            reply_listen(source, connection[i].snd.lsock, i);
        }
    } else {
        if(connection[i].snd.size != -1) {
            fprintf(stderr, "NCP: Completing outgoing RFC.\n");
            reply_open(source, connection[i].rcv.rsock, i);
        }
    }

    return 9;
}

static int process_str(uint8_t source, uint8_t *data) {
    int i;
    uint32_t lsock, rsock;
    rsock = sock(&data[0]);
    lsock = sock(&data[4]);

    fprintf(stderr, "NCP: Recieved STR %u:%u from %03o.\n",
                     lsock, rsock, source);

    if(data[8] < LINK_MIN || data[8] > LINK_MAX) {
        ncp_err(source, ERR_PARAM, data - 1, 10);
        return 9;
    }

    if(find_listen(lsock) == -1) {
        i = find_sockets(source, lsock, rsock);
        if(i == -1) {
            fprintf(stderr, "NCP: Not listening to %u, no outgoing RFC, rejecting.\n", lsock);
            ncp_err(source, ERR_CONNECT, data - 1, 10);
            return 9;
        }
        fprintf(stderr, "NCP: Outgoing RFC socket %u.\n", lsock);
    } else {
        i = find_socket(source, lsock - 1);
        if(i == -1) {
            i = make_open(source, lsock, rsock, 0, 0);
            fprintf(stderr, "NCP: Listening to %u: new connection %d.\n", lsock, i);
        } else {
            connection[i].rcv.lsock = lsock;
            connection[i].rcv.rsock = rsock;
            fprintf(stderr, "NCP: Listening to %u: connection %d.\n", lsock, i);
        }
    }
    connection[i].snd.size = data[8]; //Receive byte size.
    if(connection[i].rcv.link == -1) {
        connection[i].rcv.link = 42; //Receive link.
        ncp_rts(connection[i].host, lsock, rsock, connection[i].rcv.link);
        if(connection[i].rcv.size != -1) {
            fprintf(stderr, "NCP: Completing incoming RFC.\n");
            reply_listen(source, connection[i].snd.lsock, i);
        }
    } else {
        if(connection[i].snd.link != -1) {
            fprintf(stderr, "NCP: Completing outgoing RFC.\n");
            reply_open(source, connection[i].rcv.rsock, i);
        }
    }

    return 9;
}

static int process_cls(uint8_t source, uint8_t *data) {
    int i;
    uint32_t lsock, rsock;
    rsock = sock(&data[0]);
    lsock = sock(&data[4]);
    i = find_sockets(source, lsock, rsock);
    if(i == -1) {
        ncp_err(source, ERR_SOCKET, data - 1, 9);
        return 8;
    }
    if(connection[i].rcv.lsock == lsock)
        connection[i].rcv.lsock = connection[i].rcv.rsock = 0;
    if(connection[i].snd.lsock == lsock)
        connection[i].snd.lsock = connection[i].snd.rsock = 0;

    if(connection[i].snd.size == -1) {
        // Remote confirmed closing.
        if(connection[i].rcv.lsock == 0 && connection[i].snd.lsock == 0) {
            fprintf(stderr, "NCP: Connection %u confirmed closed.\n", i);
            listening[i].sock = 0;
            destroy(i);
            reply_close(i);
        }
    } else {
        // Remote closed connection.
        ncp_cls(connection[i].host, lsock, rsock);
        if(connection[i].rcv.lsock == 0 && connection[i].snd.lsock == 0) {
            fprintf(stderr, "NCP: Connection %u closed by remote.\n", i);
            destroy(i);
            reply_close(i);
        }
    }

    return 8;
}

static int process_all(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: Recieved ALL from %03o, link %u.\n",
                     source, data[0]);
    i = find_link(source, data[0]);
    if(i == -1)
        ncp_err(source, ERR_SOCKET, data - 1, 10);
    return 9;
}

static int process_gvb(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: Recieved GBV from %03o, link %u.",
                     source, data[0]);
    i = find_link(source, data[0]);
    if(i == -1)
        ncp_err(source, ERR_SOCKET, data - 1, 4);
    return 3;
}

static int process_ret(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: Recieved RET from %03o, link %u.",
                     source, data[0]);
    i = find_link(source, data[0]);
    if(i == -1)
        ncp_err(source, ERR_SOCKET, data - 1, 8);
    return 7;
}

static int process_inr(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: Recieved INR from %03o, link %u.",
                     source, data[0]);
    i = find_link(source, data[0]);
    if(i == -1)
        ncp_err(source, ERR_SOCKET, data - 1, 2);
    return 1;
}

static int process_ins(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: Recieved INS from %03o, link %u.",
                     source, data[0]);
    i = find_link(source, data[0]);
    if(i == -1)
        ncp_err(source, ERR_SOCKET, data - 1, 2);
    return 1;
}

static int process_eco(uint8_t source, uint8_t *data) {
    fprintf(stderr, "NCP: recieved ECO %03o from %03o, replying ERP %03o.\n",
                     *data, source, *data);
    ncp_erp(source, *data);
    return 1;
}

static void reply_echo(int i, uint8_t host, uint8_t data, uint8_t error) {
    uint8_t reply[4];
    reply[0] = WIRE_ECHO+1;
    reply[1] = host;
    reply[2] = data;
    reply[3] = error;
    if(sendto(fd, reply, sizeof reply, 0,
                         (struct sockaddr *)&connection[i].client,
                            connection[i].len) == -1)
        fprintf(stderr, "NCP: sendto %s error: %s.\n",
                         connection[i].client.sun_path, strerror(errno));
}

static int process_erp(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: recieved ERP %03o from %03o.\n",
                     *data, source);
    i = find_link(source, LINK_ECHO);
    if(i == -1) {
        fprintf(stderr, "NCP: No ongoing ECO.\n");
        return 1;
    }
    reply_echo(i, source, *data, 0x10);
    destroy(i);
    return 1;
}

static int process_err(uint8_t source, uint8_t *data) {
    uint32_t rsock;
    int i;
    const char *meaning;
    switch(*data) {
    case ERR_UNDEFINED: meaning = "Undefined"; break;
    case ERR_OPCODE:        meaning = "Illegal opcode"; break;
    case ERR_SHORT:         meaning = "Short parameter space"; break;
    case ERR_PARAM:         meaning = "Bad parameters"; break;
    case ERR_SOCKET:        meaning = "Request on a non-existent socket"; break;
    case ERR_CONNECT:     meaning = "Socket(link) not connected"; break;
    default: meaning = "Unknown"; break;
    }
    fprintf(stderr, "NCP: recieved ERR code %03o from %03o: %s.\n",
                     *data, source, meaning);
    fprintf(stderr, "NCP: error data:");
    for(i = 1; i < 11; i++)
        fprintf(stderr, " %03o", data[i]);
    fprintf(stderr, "\n");

    if((data[0] == ERR_SOCKET || data[0] == ERR_CONNECT) &&
         (data[1] == NCP_RTS || data[1] == NCP_STR)) {
        rsock = sock(data + 6);
        i = find_sockets(source, sock(data + 2), rsock);
        if(i != -1) {
            if((rsock & 1) == 0)
                rsock--;
            reply_open(source, rsock, 255);
            destroy(i);
        }
    }

    return 11;
}

static int process_rst(uint8_t source, uint8_t *data) {
    int i;
    fprintf(stderr, "NCP: recieved RST from %03o.\n", source);
    for(i = 0; i < CONNECTIONS; i++) {
        if(connection[i].host != source)
            continue;
        destroy(i);
    }
    ncp_rrp(source);
    return 0;
}

static int process_rrp(uint8_t source, uint8_t *data) {
    fprintf(stderr, "NCP: recieved RRP from %03o.\n", source);
    return 0;
}

static int(*ncp_messages[])(uint8_t source, uint8_t *data) = {
    process_nop,
    process_rts,
    process_str,
    process_cls,
    process_all,
    process_gvb,
    process_ret,
    process_inr,
    process_ins,
    process_eco,
    process_erp,
    process_err,
    process_rst,
    process_rrp
};

static void process_ncp(uint8_t source, uint8_t *data, uint16_t count) {
    int i = 0, n;
    while(i < count) {
        uint8_t type = data[i++];
        if(type > NCP_MAX) {
            ncp_err(source, ERR_OPCODE, data - 1, 10);
            return;
        }
        n = ncp_messages[type](source, &data[i]);
        if(i + n > count)
            ncp_err(source, ERR_SHORT, data - 1, count - i + 1);
        i += n;
    }
}

static void reply_read(uint8_t connection, uint8_t *data, int n) {
    static uint8_t reply[1000];
    reply[0] = WIRE_READ+1;
    reply[1] = connection;
    memcpy(reply + 2, data, n);
    if(sendto(fd, reply, n + 2, 0,(struct sockaddr *)&client, len) == -1)
        fprintf(stderr, "NCP: sendto %s error: %s.\n",
                         client.sun_path, strerror(errno));
}

static void process_regular(uint8_t *packet, int length) {
    uint8_t source = packet[1];
    uint8_t link = packet[2];
    int i;

    if(link == 0) {
        uint16_t count =(packet[6] << 8) | packet[7];
        process_ncp(source, &packet[9], count);
    } else {
        fprintf(stderr, "NCP: process regular from %03o link %u.\n",
                         source, link);
        i = find_link(source, link);
        if(i == -1) {
            fprintf(stderr, "NCP: Link not connected.\n");
            return;
        }
        length = 2 *(length - 2);
        fprintf(stderr, "NCP: Connection %u, length %u.\n", i, length);
        reply_read(i, packet + 4, length);
    }
}

static void process_leader_error(uint8_t *packet, int length) {
    const char *reason;
    switch(packet[3] & 0x0F) {
    case 0: reason = "IMP error during leader"; break;
    case 1: reason = "Message less than 32 bits"; break;
    case 2: reason = "Illegal type"; break;
    default: reason = "Unknown reason"; break;
    }
    fprintf(stderr, "NCP: Error in leader: %s.\n", reason);
}

static void process_imp_down(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: IMP going down.\n");
}

static void process_blocked(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: Blocked link.\n");
}

static void process_imp_nop(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: NOP.\n");
}

static void process_rfnm(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: Ready for next message to host %03o link %u.\n",
                     packet[1], packet[2]);
}

static void process_full(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: Link table full.\n");
}

static void process_host_dead(uint8_t *packet, int length) {
    int i;
    const char *reason;
    switch(packet[3] & 0x0F) {
    case 0: reason = "IMP cannot be reached"; break;
    case 1: reason = "is not up"; break;
    case 3: reason = "communication administratively prohibited"; break;
    default: reason = "dead, unknown reason"; break;
    }
    fprintf(stderr, "NCP: Host %03o %s.\n", packet[1], reason);

    i = find_link(packet[1], LINK_ECHO);
    if(i != -1) {
        reply_echo(i, packet[1], 0, packet[3] & 0x0F);
        destroy(i);
    }
}

static void process_data_error(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: Error in data.\n");
}

static void process_incomplete(uint8_t *packet, int length) {
    const char *reason;
    switch(packet[3] & 0x0F) {
    case 0: reason = "Host did not accept message quickly enough"; break;
    case 1: reason = "Message too long"; break;
    case 2: reason = "Message took too long in transmission"; break;
    case 3: reason = "Message lost in network"; break;
    case 4: reason = "Resources unavailable"; break;
    case 5: reason = "I/O failure during reception"; break;
    default: reason = "Unknown reason"; break;
    }
    fprintf(stderr, "NCP: Incomplete transmission from %03o: %s.\n",
                     packet[1], reason);
}

static void process_reset(uint8_t *packet, int length) {
    fprintf(stderr, "NCP: IMP reset.\n");
}

static void(*imp_messages[])(uint8_t *packet, int length) = {
    process_regular,
    process_leader_error,
    process_imp_down,
    process_blocked,
    process_imp_nop,
    process_rfnm,
    process_full,
    process_host_dead,
    process_data_error,
    process_incomplete,
    process_reset
};

static void process_imp(uint8_t *packet, int length) {
    int type;

#if 1
    {
        int i;
        for(i = 0; i < 2 * length; i+=2)
            fprintf(stderr, " >>> %06o(%03o %03o)\n",
                            (packet[i] << 8) | packet[i+1], packet[i], packet[i+1]);
    }
#endif

    if(length < 2) {
        fprintf(stderr, "NCP: leader too short.\n");
        send_leader_error(1);
        return;
    }
    type = packet[0] & 0x0F;
    if(type <= IMP_RESET)
        imp_messages[type](packet, length);
    else {
        fprintf(stderr, "NCP: leader type bad.\n");
        send_leader_error(2);
    }
}

static void send_nops(void) {
    send_nop();
    sleep(1);
    send_nop();
    sleep(1);
    send_nop();
}

static void ncp_reset(int flap) {
    fprintf(stderr, "NCP: Reset.\n");
    if(flap) {
        fprintf(stderr, "NCP: Flap host ready.\n");
        imp_host_ready(0);
        imp_host_ready(1);
    }

    send_nops();
}

static int imp_ready = 0;

static void ncp_imp_ready(int flag) {
    if(!imp_ready && flag) {
        fprintf(stderr, "NCP: IMP going up.\n");
        //ncp_reset(0);
    } else if(imp_ready && !flag) {
        fprintf(stderr, "NCP: IMP going down.\n");
    }
    imp_ready = flag;
}

static void app_echo(void) {
    int i;
    fprintf(stderr, "NCP: Application echo.\n");
    i = find_link(-1, -1);
    if(i == -1) {
        fprintf(stderr, "NCP: Table full.\n");
        return;
    }
    connection[i].host = app[1];
    connection[i].rcv.link = LINK_ECHO;
    memcpy(&connection[i].client, &client, len);
    connection[i].len = len;
    ncp_eco(app[1], app[2]);
}

static void app_open(void) {
    uint32_t socket;
    int i;

    socket = app[2] << 24 | app[3] << 16 | app[4] << 8 | app[5];
    fprintf(stderr, "NCP: Application open sockets %u,%u on host %03o.\n",
                     socket, socket+1, app[1]);

    // Initiate a connection.
    i = make_open(app[1], 1002, socket, 1003, socket+1);
    connection[i].rcv.link = 42; //Receive link.
    connection[i].rcv.size = 8;    //Send byte size.
    memcpy(&connection[i].client, &client, len);
    connection[i].len = len;

    // Send RFC messages.
    ncp_rts(connection[i].host, connection[i].rcv.lsock,
                     connection[i].rcv.rsock, connection[i].rcv.link);
    ncp_str(connection[i].host, connection[i].snd.lsock,
                     connection[i].snd.rsock, connection[i].rcv.size);
}

static void app_listen(void) {
    uint32_t socket;
    int i;

    socket = app[1] << 24 | app[2] << 16 | app[3] << 8 | app[4];
    fprintf(stderr, "NCP: Application listen to socket %u.\n", socket);
    if(find_listen(socket) != -1) {
        fprintf(stderr, "NCP: Alreay listening to %d.\n", socket);
        reply_listen(0, socket, 0);
        return;
    }
    i = find_listen(0);
    if(i == -1) {
        fprintf(stderr, "NCP: Table full.\n");
        reply_listen(0, socket, 0);
        return;
    }
    listening[i].sock = socket;
    memcpy(&connection[i].client, &client, len);
    connection[i].len = len;
}

static void app_read(void) {
    int i;
    i = app[1];
    fprintf(stderr, "NCP: Application read %u octets from connection %u.\n",
                     app[2], i);
    ncp_all(connection[i].host, connection[i].rcv.link, 1, 8 * app[2]);
}

static void reply_write(uint8_t connection) {
    uint8_t reply[2];
    reply[0] = WIRE_WRITE+1;
    reply[1] = connection;
    if(sendto(fd, reply, sizeof reply, 0,(struct sockaddr *)&client, len) == -1)
        fprintf(stderr, "NCP: sendto %s error: %s.\n",
                         client.sun_path, strerror(errno));
}

static void app_write(int n) {
    int i = app[1];
    fprintf(stderr, "NCP: Application write, %u bytes to connection %u.\n",
                     n, i);
    send_imp(0, IMP_REGULAR, connection[i].host, connection[i].snd.link, 0, 0,
                        app + 2, 2 +(n + 1) / 2);
    reply_write(i);
}

static void app_interrupt(void) {
    int i = app[1];
    fprintf(stderr, "NCP: Application interrupt, connection %u.\n", i);
    ncp_ins(connection[i].host, connection[i].snd.link);
}

static void app_close(void) {
    int i = app[1];
    fprintf(stderr, "NCP: Application close, connection %u.\n", i);
    connection[i].snd.size = connection[i].rcv.size = -1;
    ncp_cls(connection[i].host, connection[i].rcv.lsock, connection[i].rcv.rsock);
    ncp_cls(connection[i].host, connection[i].snd.lsock, connection[i].snd.rsock);
}

static void application(void) {
    ssize_t n;

    len = sizeof client;
    n = recvfrom(fd, app, sizeof app, 0,(struct sockaddr *)&client, &len);
    if(n == -1) {
        fprintf(stderr, "NCP: recvfrom error.\n");
        return;
    }

    fprintf(stderr, "NCP: Received application request %u from %s.\n",
        app[0], client.sun_path);

    if(!wire_check(app[0], n)) {
        fprintf(stderr, "NCP: bad application request.\n");
        return;
    }

    switch(app[0]) {
    case WIRE_ECHO:             app_echo(); break;
    case WIRE_OPEN:             app_open(); break;
    case WIRE_LISTEN:         app_listen(); break;
    case WIRE_READ:             app_read(); break;
    case WIRE_WRITE:      app_write(n - 2); break;
    case WIRE_INTERRUPT:   app_interrupt(); break;
    case WIRE_CLOSE:           app_close(); break;
    default: fprintf(stderr, "NCP: bad application request.\n"); break;
    }
}

static void cleanup(void) {
    unlink(server.sun_path);
}

void ncp_init(void) {
    char *path;
    int i;

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    memset(&server, 0, sizeof server);
    server.sun_family = AF_UNIX;
    path = getenv("NCP");
    strncpy(server.sun_path, path, sizeof server.sun_path - 1);
    if(bind(fd,(struct sockaddr *)&server, sizeof server) == -1) {
        fprintf(stderr, "NCP: bind error: %s.\n", strerror(errno));
        fprintf(stderr, "Is $NCP set to the path to a domain socket? If so, run 'rm $NCP' before retrying.\n");
        exit(1);
    }
    atexit(cleanup);

    for(i = 0; i < CONNECTIONS; i ++) {
        destroy(i);
        listening[i].sock = 0;
    }
}

int main(int argc, char **argv) {
    imp_init(argc, argv);
    ncp_init();
    imp_imp_ready = ncp_imp_ready;
    imp_host_ready(1);
    ncp_reset(0);
    for(;;) {
        int n;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        imp_fd_set(&rfds);
        n = select(33, &rfds, NULL, NULL, NULL);
        if(n == -1)
            fprintf(stderr, "NCP: select error.\n");
        else if(n > 0) {
            if(imp_fd_isset(&rfds)) {
                memset(packet, 0, sizeof packet);
                imp_receive_message(packet, &n);
                if(n > 0)
                    process_imp(packet, n);
            }
            if(FD_ISSET(fd, &rfds)) {
                application();
            }
        }
    }
}
