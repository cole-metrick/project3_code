/*
 * CS 1652 Project 3
 * (c) Amy Babay, 2022
 * (c) <Student names here>
 *
 * Computer Science Department
 * University of Pittsburgh
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/errno.h>

#include <spu_alarm.h>
#include <spu_events.h>

#include "packets.h"
#include "client_list.h"
#include "node_list.h"
#include "edge_list.h"

#define PRINT_DEBUG 0

#define MAX_CONF_LINE 1024

enum mode
{
    MODE_NONE,
    MODE_LINK_STATE,
    MODE_DISTANCE_VECTOR,
};

static uint32_t My_IP = 0;
static uint32_t My_ID = 0;
static uint16_t My_Port = 0;

static enum mode Route_Mode = MODE_NONE;

static struct client_list Client_List;
static struct node_list Node_List;
static struct edge_list Edge_List;

static int Client_Sock = -1;
static int Ctrl_Sock = -1;
static int Data_Sock = -1;

//initializing needed variables
int active[MAX_NODES];
int edges[MAX_NODES][MAX_NODES];
int updates[MAX_NODES];
int updateNum = 1;
int fwdingTable[MAX_NODES];
static const sp_time heartbeat_timer = {10, 0};
static const sp_time Data_Timer = {1, 0};


//initializing helper methids
void dead_link(int neighbor, void *unused);
void send_heartbeat(int neighbor_id, void *unused);
void dijkstras(int startingId, int forward, int update, int src, int updateNum, int updates[MAX_NODES]);
void send_link_state(int neighbor_id, int update, int firstSend, int src, int messageUpdNum, int updates[MAX_NODES]);

/* Forward the packet to the next-hop node based on forwarding table */
void forward_data(struct data_pkt *pkt) {
    Alarm(DEBUG, "overlay_node: forwarding data to overlay node %u, client port "
                 "%u\n",
          pkt->hdr.dst_id, pkt->hdr.dst_port);
    /*
     * Students fill in! Do forwarding table lookup, update path information in
     * header (see deliver_locally for an example), and send packet to next hop
     * */
    
    //forwarding table lookup
    int hopID = fwdingTable[pkt->hdr.dst_id];
    if (hopID == INT_MAX)
    {
        Alarm(PRINT, "sending to inactive node");
        return;
    }
    
    //update path info
    int path_len = 0;
    path_len = pkt->hdr.path_len;
    if (path_len < MAX_PATH)
    {
        pkt->hdr.path[path_len] = My_ID;
        pkt->hdr.path_len++;
    }
    
    //send packet to hop
    struct node *node = get_node_from_id(&Node_List, hopID);
    int ret = sendto(Data_Sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&node->data_addr, sizeof(node->data_addr));
    if (ret < 0)
    {
        Alarm(PRINT, "Error forwarding data");
        remove_client_with_sock(&Client_List, Ctrl_Sock);
    }
}

/* Deliver packet to one of my local clients */
void deliver_locally(struct data_pkt *pkt)
{
    int path_len = 0;
    int bytes = 0;
    int ret = -1;
    struct client_conn *c = get_client_from_port(&Client_List, pkt->hdr.dst_port);

    /* Check whether we have a local client with this port to deliver to. If
     * not, nothing to do */
    if (c == NULL)
    {
        Alarm(PRINT, "overlay_node: received data for client that does not "
                     "exist! overlay node %d : client port %u\n",
              pkt->hdr.dst_id, pkt->hdr.dst_port);
        return;
    }

    Alarm(DEBUG, "overlay_node: Delivering data locally to client with local "
                 "port %d\n",
          c->data_local_port);

    /* stamp packet so we can see the path taken */
    path_len = pkt->hdr.path_len;
    if (path_len < MAX_PATH)
    {
        pkt->hdr.path[path_len] = My_ID;
        pkt->hdr.path_len++;
    }

    /* Send data to client */
    bytes = sizeof(struct data_pkt) - MAX_PAYLOAD_SIZE + pkt->hdr.data_len;
    ret = sendto(c->data_sock, pkt, bytes, 0,
                 (struct sockaddr *)&c->data_remote_addr,
                 sizeof(c->data_remote_addr));
    if (ret < 0)
    {
        Alarm(PRINT, "Error sending to client with sock %d %d:%d\n",
              c->data_sock, c->data_local_port, c->data_remote_port);
        goto err;
    }

    return;

err:
    remove_client_with_sock(&Client_List, c->control_sock);
}

/* Handle incoming data message from another overlay node. Check whether we
 * need to deliver locally to a connected client, or forward to the next hop
 * overlay node */
void handle_overlay_data(int sock, int code, void *data)
{
    int bytes;
    struct data_pkt pkt;
    struct sockaddr_in recv_addr;
    socklen_t fromlen;

    Alarm(DEBUG, "overlay_node: received overlay data msg!\n");

    fromlen = sizeof(recv_addr);
    bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&recv_addr,
                     &fromlen);
    if (bytes < 0)
    {
        Alarm(EXIT, "overlay node: Error receiving overlay data: %s\n",
              strerror(errno));
    }

    /* If there is data to forward, find next hop and forward it */
    if (pkt.hdr.data_len > 0)
    {
        { /* log packet for debugging */
            char tmp_payload[MAX_PAYLOAD_SIZE + 1];
            memcpy(tmp_payload, pkt.payload, pkt.hdr.data_len);
            tmp_payload[pkt.hdr.data_len] = '\0';
            Alarm(DEBUG, "Got forwarded data packet of %d bytes: %s\n",
                  pkt.hdr.data_len, tmp_payload);
        }

        if (pkt.hdr.dst_id == My_ID)
        {
            deliver_locally(&pkt);
        }
        else
        {
            forward_data(&pkt);
        }
    }
}

/* Respond to heartbeat message by sending heartbeat echo */
void handle_heartbeat(struct heartbeat_pkt *pkt) {
    if (pkt->hdr.type != CTRL_HEARTBEAT)
    {
        Alarm(PRINT, "Error: non-heartbeat msg in handle_heartbeat\n");
        return;
    }

    Alarm(DEBUG, "Got heartbeat from %d\n", pkt->hdr.src_id);

    /* Students fill in! */

    //create heartbeat message
    struct heartbeat_echo_pkt echoPkt;
    echoPkt.hdr.type = CTRL_HEARTBEAT_ECHO;
    echoPkt.hdr.src_id = My_ID;
    echoPkt.hdr.dst_id = pkt->hdr.src_id;

    //send heartbeat
    struct node *node = get_node_from_id(&Node_List, pkt->hdr.src_id);
    int ret = sendto(Ctrl_Sock, &echoPkt, sizeof(echoPkt), 0, (struct sockaddr *)&node->ctrl_addr, sizeof(node->ctrl_addr));
    if (ret < 0)
    {
        Alarm(PRINT, "Error sending heartbeat echo");
        remove_client_with_sock(&Client_List, Ctrl_Sock);
    }

    return;
}

/* Handle heartbeat echo. This indicates that the link is alive, so update our
 * link weights and send update if we previously thought this link was down.
 * Push forward timer for considering the link dead */
void handle_heartbeat_echo(struct heartbeat_echo_pkt *pkt)
{
    if (pkt->hdr.type != CTRL_HEARTBEAT_ECHO)
    {
        Alarm(PRINT, "Error: non-heartbeat_echo msg in "
                     "handle_heartbeat_echo\n");
        return;
    }

    Alarm(DEBUG, "Got heartbeat_echo from %d\n", pkt->hdr.src_id);

    /* Students fill in! */

    //update the active status of the source
    int activeStatus = active[pkt->hdr.src_id];
    active[pkt->hdr.src_id] = 1;

    //if it was thought to be inactive
    if (activeStatus == 0)
    {
        if (Route_Mode == MODE_LINK_STATE)
        {
            //mark edges as active and update the dijkstras
            edges[My_ID][pkt->hdr.src_id] = 1;
            edges[pkt->hdr.src_id][My_ID] = 1;
            
            // Alarm(PRINT, "got to dijkstras echo\n");
            dijkstras(My_ID, 1, 1, 0, 0, updates);
        }
        else
        {
            // distance vector implemetation
        }
    }

    //start a heartbeat timer
    E_queue(dead_link, pkt->hdr.src_id, &My_ID, heartbeat_timer);
}

/* Process received link state advertisement */
void handle_lsa(struct lsa_pkt *pkt)
{
    if (pkt->hdr.type != CTRL_LSA)
    {
        Alarm(PRINT, "Error: non-lsa msg in handle_lsa\n");
        return;
    }

    if (Route_Mode != MODE_LINK_STATE)
    {
        Alarm(PRINT, "Error: LSA msg but not in link state routing mode\n");
    }

    Alarm(DEBUG, "Got lsa from %d\n", pkt->hdr.src_id);

    /* Students fill in! */

    //flood if new update
    if (updates[pkt->hdr.src_id] < pkt->updateNum)
    {
        updates[pkt->hdr.src_id] = pkt->updateNum;
        for (int i = 0; i < MAX_NODES; i++)
        {
            edges[pkt->hdr.src_id][i] = pkt->updateRow[i];
            edges[i][pkt->hdr.src_id] = pkt->updateRow[i];
            if (pkt->updateRow[i] == 0)
            {
                updates[i] = 0;
            }
        }

        // Alarm(PRINT, "got to dijkstras lsa\n");
        dijkstras(My_ID, 1, 0, pkt->hdr.src_id, pkt->updateNum, pkt->updateRow);
    }
}

/* Process received distance vector update */
void handle_dv(struct dv_pkt *pkt)
{
    if (pkt->hdr.type != CTRL_DV)
    {
        Alarm(PRINT, "Error: non-dv msg in handle_dv\n");
        return;
    }

    if (Route_Mode != MODE_DISTANCE_VECTOR)
    {
        Alarm(PRINT, "Error: Distance Vector Update msg but not in distance "
                     "vector routing mode\n");
    }

    Alarm(DEBUG, "Got dv from %d\n", pkt->hdr.src_id);

    /* Students fill in! */
}

void dead_link(int neighbor, void *unused) {
    //mark neighbor as inactive
    active[neighbor] = 0;

    if (Route_Mode == MODE_LINK_STATE)
    {
        //mark edges as inactive and update dijkstras
        edges[My_ID][neighbor] = 0;
        edges[neighbor][My_ID] = 0;

        // Alarm(PRINT, "Got to dijkstras dead_link \n");
        dijkstras(My_ID, 1, 1, 0, 0, updates);
    } else {
        // distance vector
    }
}

/* Process received overlay control message. Identify message type and call the
 * relevant "handle" function */
void handle_overlay_ctrl(int sock, int code, void *data)
{
    char buf[MAX_CTRL_SIZE];
    struct sockaddr_in recv_addr;
    socklen_t fromlen;
    struct ctrl_hdr *hdr = NULL;
    int bytes = 0;

    Alarm(DEBUG, "overlay_node: received overlay control msg!\n");

    fromlen = sizeof(recv_addr);
    bytes = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&recv_addr,
                     &fromlen);
    if (bytes < 0)
    {
        Alarm(EXIT, "overlay node: Error receiving ctrl message: %s\n",
              strerror(errno));
    }
    hdr = (struct ctrl_hdr *)buf;

    /* sanity check */
    if (hdr->dst_id != My_ID)
    {
        Alarm(PRINT, "overlay_node: Error: got ctrl msg with invalid dst_id: "
                     "%d\n",
              hdr->dst_id);
    }

    if (hdr->type == CTRL_HEARTBEAT)
    {
        /* handle heartbeat */
        handle_heartbeat((struct heartbeat_pkt *)buf);
    }
    else if (hdr->type == CTRL_HEARTBEAT_ECHO)
    {
        /* handle heartbeat echo */
        handle_heartbeat_echo((struct heartbeat_echo_pkt *)buf);
    }
    else if (hdr->type == CTRL_LSA)
    {
        /* handle link state update */
        handle_lsa((struct lsa_pkt *)buf);
    }
    else if (hdr->type == CTRL_DV)
    {
        /* handle distance vector update */
        handle_dv((struct dv_pkt *)buf);
    }
}

void handle_client_data(int sock, int unused, void *data)
{
    int ret, bytes;
    struct data_pkt pkt;
    struct sockaddr_in recv_addr;
    socklen_t fromlen;
    struct client_conn *c;

    Alarm(DEBUG, "Handle client data\n");

    c = (struct client_conn *)data;
    if (sock != c->data_sock)
    {
        Alarm(EXIT, "Bad state! sock %d != data sock\n", sock, c->data_sock);
    }

    fromlen = sizeof(recv_addr);
    bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&recv_addr,
                     &fromlen);
    if (bytes < 0)
    {
        Alarm(PRINT, "overlay node: Error receiving from client: %s\n",
              strerror(errno));
        goto err;
    }

    /* Special case: initial data packet from this client. Use it to set the
     * source port, then ack it */
    if (c->data_remote_port == 0)
    {
        c->data_remote_addr = recv_addr;
        c->data_remote_port = ntohs(recv_addr.sin_port);
        Alarm(DEBUG, "Got initial data msg from client with sock %d local port "
                     "%u remote port %u\n",
              sock, c->data_local_port,
              c->data_remote_port);

        /* echo pkt back to acknowledge */
        ret = sendto(c->data_sock, &pkt, bytes, 0,
                     (struct sockaddr *)&c->data_remote_addr,
                     sizeof(c->data_remote_addr));
        if (ret < 0)
        {
            Alarm(PRINT, "Error sending to client with sock %d %d:%d\n", sock,
                  c->data_local_port, c->data_remote_port);
            goto err;
        }
    }

    /* If there is data to forward, find next hop and forward it */
    if (pkt.hdr.data_len > 0)
    {
        { /* log packet for debugging */
            char tmp_payload[MAX_PAYLOAD_SIZE + 1];
            memcpy(tmp_payload, pkt.payload, pkt.hdr.data_len);
            tmp_payload[pkt.hdr.data_len] = '\0';
            Alarm(DEBUG, "Got data packet of %d bytes: %s\n", pkt.hdr.data_len, tmp_payload);
        }

        /* Set up header with my info */
        pkt.hdr.src_id = My_ID;
        pkt.hdr.src_port = c->data_local_port;

        /* Deliver / Forward */
        if (pkt.hdr.dst_id == My_ID)
        {
            deliver_locally(&pkt);
        }
        else
        {
            forward_data(&pkt);
        }
    }

    return;

err:
    remove_client_with_sock(&Client_List, c->control_sock);
}

void handle_client_ctrl_msg(int sock, int unused, void *data)
{
    int bytes_read = 0;
    int bytes_sent = 0;
    int bytes_expected = sizeof(struct conn_req_pkt);
    struct conn_req_pkt rcv_req;
    struct conn_ack_pkt ack;
    int ret = -1;
    int ret_code = 0;
    char *err_str = "client closed connection";
    struct sockaddr_in saddr;
    struct client_conn *c;

    Alarm(DEBUG, "Client ctrl message, sock %d\n", sock);

    /* Get client info */
    c = (struct client_conn *)data;
    if (sock != c->control_sock)
    {
        Alarm(EXIT, "Bad state! sock %d != data sock\n", sock, c->control_sock);
    }

    if (c == NULL)
    {
        Alarm(PRINT, "Failed to find client with sock %d\n", sock);
        ret_code = -1;
        goto end;
    }

    /* Read message from client */
    while (bytes_read < bytes_expected &&
           (ret = recv(sock, ((char *)&rcv_req) + bytes_read,
                       sizeof(rcv_req) - bytes_read, 0)) > 0)
    {
        bytes_read += ret;
    }
    if (ret <= 0)
    {
        if (ret < 0)
            err_str = strerror(errno);
        Alarm(PRINT, "Recv returned %d; Removing client with control sock %d: "
                     "%s\n",
              ret, sock, err_str);
        ret_code = -1;
        goto end;
    }

    if (c->data_local_port != 0)
    {
        Alarm(PRINT, "Received req from already connected client with sock "
                     "%d\n",
              sock);
        ret_code = -1;
        goto end;
    }

    /* Set up UDP socket requested for this client */
    if ((c->data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        Alarm(PRINT, "overlay_node: client UDP socket error: %s\n", strerror(errno));
        ret_code = -1;
        goto send_resp;
    }

    /* set server address */
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(rcv_req.port);

    /* bind UDP socket */
    if (bind(c->data_sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        Alarm(PRINT, "overlay_node: client UDP bind error: %s\n", strerror(errno));
        ret_code = -1;
        goto send_resp;
    }

    /* Register socket with event handling system */
    ret = E_attach_fd(c->data_sock, READ_FD, handle_client_data, 0, c, MEDIUM_PRIORITY);
    if (ret < 0)
    {
        Alarm(PRINT, "Failed to register client UDP sock in event handling system\n");
        ret_code = -1;
        goto send_resp;
    }

send_resp:
    /* Send response */
    if (ret_code == 0)
    { /* all worked correctly */
        c->data_local_port = rcv_req.port;
        ack.id = My_ID;
    }
    else
    {
        ack.id = 0;
    }
    bytes_expected = sizeof(ack);
    Alarm(DEBUG, "Sending response to client with control sock %d, UDP port "
                 "%d\n",
          sock, c->data_local_port);
    while (bytes_sent < bytes_expected)
    {
        ret = send(sock, ((char *)&ack) + bytes_sent, sizeof(ack) - bytes_sent, 0);
        if (ret < 0)
        {
            Alarm(PRINT, "Send error for client with sock %d (removing...): "
                         "%s\n",
                  sock, strerror(ret));
            ret_code = -1;
            goto end;
        }
        bytes_sent += ret;
    }

end:
    if (ret_code != 0 && c != NULL)
        remove_client_with_sock(&Client_List, sock);
}

void handle_client_conn(int sock, int unused, void *data)
{
    int conn_sock;
    struct client_conn new_conn;
    struct client_conn *ret_conn;
    int ret;

    Alarm(DEBUG, "Handle client connection\n");

    /* Accept the connection */
    conn_sock = accept(sock, NULL, NULL);
    if (conn_sock < 0)
    {
        Alarm(PRINT, "accept error: %s\n", strerror(errno));
        goto err;
    }

    /* Set up the connection struct for this new client */
    new_conn.control_sock = conn_sock;
    new_conn.data_sock = -1;
    new_conn.data_local_port = 0;
    new_conn.data_remote_port = 0;
    ret_conn = add_client_to_list(&Client_List, new_conn);
    if (ret_conn == NULL)
    {
        goto err;
    }

    /* Register the control socket for this client */
    ret = E_attach_fd(new_conn.control_sock, READ_FD, handle_client_ctrl_msg,
                      0, ret_conn, MEDIUM_PRIORITY);
    if (ret < 0)
    {
        goto err;
    }

    return;

err:
    if (conn_sock >= 0)
        close(conn_sock);
}

void init_overlay_data_sock(int port)
{
    int ret = -1;
    struct sockaddr_in saddr;

    if ((Data_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        Alarm(EXIT, "overlay_node: data socket error: %s\n", strerror(errno));
    }

    /* set server address */
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(port);

    /* bind listening socket */
    if (bind(Data_Sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        Alarm(EXIT, "overlay_node: data bind error: %s\n", strerror(errno));
    }

    /* Register socket with event handling system */
    ret = E_attach_fd(Data_Sock, READ_FD, handle_overlay_data, 0, NULL, MEDIUM_PRIORITY);
    if (ret < 0)
    {
        Alarm(EXIT, "Failed to register overlay data sock in event handling system\n");
    }
}

void init_overlay_ctrl_sock(int port)
{
    int ret = -1;
    struct sockaddr_in saddr;

    if ((Ctrl_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        Alarm(EXIT, "overlay_node: ctrl socket error: %s\n", strerror(errno));
    }

    /* set server address */
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(port);

    /* bind listening socket */
    if (bind(Ctrl_Sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        Alarm(EXIT, "overlay_node: ctrl bind error: %s\n", strerror(errno));
    }

    /* Register socket with event handling system */
    ret = E_attach_fd(Ctrl_Sock, READ_FD, handle_overlay_ctrl, 0, NULL, MEDIUM_PRIORITY);
    if (ret < 0)
    {
        Alarm(EXIT, "Failed to register overlay ctrl sock in event handling system\n");
    }
}

void init_client_sock(int client_port)
{
    int ret = -1;
    struct sockaddr_in saddr;

    if ((Client_Sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
    {
        Alarm(EXIT, "overlay_node: client socket error: %s\n", strerror(errno));
    }

    /* set server address */
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(client_port);

    /* bind listening socket */
    if (bind(Client_Sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        Alarm(EXIT, "overlay_node: client bind error: %s\n", strerror(errno));
    }

    /* start listening */
    if (listen(Client_Sock, 32) < 0)
    {
        Alarm(EXIT, "overlay_node: client bind error: %s\n", strerror(errno));
        exit(-1);
    }

    /* Register socket with event handling system */
    ret = E_attach_fd(Client_Sock, READ_FD, handle_client_conn, 0, NULL, MEDIUM_PRIORITY);
    if (ret < 0)
    {
        Alarm(EXIT, "Failed to register client sock in event handling system\n");
    }
}

void init_link_state(void)
{
    Alarm(DEBUG, "init link state\n");

    // init arrays
    for (int i = 0; i < MAX_NODES; i++)
    {
        updates[i] = 0;
        active[i] = 1;
        for (int j = 0; j < MAX_NODES; j++)
        {
            edges[i][j] = 1;
        }
    }

    // Alarm(PRINT, "got to dijkstras init\n");
    dijkstras(My_ID, 0, 0, 0, 0, updates);

    void *unused = NULL;
    for (int i = 0; i < Edge_List.num_edges; i++)
    {
        int srcID = Edge_List.edges[i]->src_id;
        int dstID = Edge_List.edges[i]->dst_id;

        if (srcID == My_ID)
        {
            send_heartbeat(dstID, unused);
            E_queue(dead_link, dstID, &My_ID, heartbeat_timer);
        }
    }
}

void send_heartbeat(int neighbor_id, void *unused) {
    
    struct heartbeat_pkt heartPkt;
    struct node *node;
    int ret = 0;
    heartPkt.hdr.dst_id = neighbor_id;
    heartPkt.hdr.src_id = My_ID;
    heartPkt.hdr.type = CTRL_HEARTBEAT;

    node = get_node_from_id(&Node_List, neighbor_id);

    // struct sockaddr_in ctrl_addr_ex = node->ctrl_addr;
    // ctrl_addr_ex.sin_port = htons(ntohs(node->ctrl_addr.sin_port) + 1);

    ret = sendto(Ctrl_Sock, &heartPkt, sizeof(heartPkt), 0, (struct sockaddr *)&node->ctrl_addr, sizeof(node->ctrl_addr));
    if (ret < 0)
    {
        Alarm(PRINT, "Error sending heartbeat to sock %d\n", Ctrl_Sock);
        goto err;
    }

    // start timer to send a heartbeat packet every second
    // Alarm(PRINT, "sent heartbeat\n");
    E_queue(send_heartbeat, neighbor_id, &heartPkt, Data_Timer);

    return;
err:
    remove_client_with_sock(&Client_List, Ctrl_Sock);
}

void dijkstras(int startingId, int forward, int update, int messageSrc, int messageUpdateNum, int updates[MAX_NODES])
{
    // Alarm(PRINT, "in dijkstras\n");
    int visitedCount = 0;
    
    int visited[MAX_NODES];
    int distance[MAX_NODES];
    
    for (int i = 0; i < MAX_NODES; i++)
    {
        visited[i] = 0;
        distance[i] = INT_MAX;
        // Alarm(PRINT, "hi\n");
        // Alarm(PRINT, "Num nodes %d\n", Node_List.num_nodes);
        fwdingTable[i] = INT_MAX;
        // Alarm(PRINT, "hi2\n");
    }
    
    visited[startingId] = 1;
    distance[startingId] = 0;
    fwdingTable[startingId] = startingId;
    visitedCount++;
    
    int nodeCount = Node_List.num_nodes;
    int edgeCount = Edge_List.num_edges;
    while (visitedCount < nodeCount)
    {
        for (int i = 0; i < edgeCount; i++)
        {
            int srcEdgeId = Edge_List.edges[i]->src_id;
            int dstEdgeId = Edge_List.edges[i]->dst_id;
            int edgeCost = Edge_List.edges[i]->cost;
            
            int visitable = (visited[srcEdgeId] == 1) && (visited[dstEdgeId] == 0);
            int available = edges[srcEdgeId][dstEdgeId];
            
            if (visitable && available)
            {
                int pathCost = distance[srcEdgeId] + edgeCost;
                
                if (distance[dstEdgeId] > pathCost)
                {
                    distance[dstEdgeId] = pathCost;
                    if (srcEdgeId == startingId)
                    {
                        fwdingTable[dstEdgeId] = dstEdgeId;
                    }
                    else
                    {
                        fwdingTable[dstEdgeId] = fwdingTable[srcEdgeId];
                    }
                }
            }
        }
        
        int currShortest = INT_MAX;
        int currShortestIndex = -1;
        for (int i = 0; i < MAX_NODES; i++)
        {
            if ((distance[i] < currShortest) && (visited[i] == 0))
            {
                currShortest = distance[i];
                currShortestIndex = i;
            }
        }
        visited[currShortestIndex] = 1;
        visitedCount++;
    }

    if (forward == 1)
    {
        for (int i = 0; i < Edge_List.num_edges; i++)
        {
            int srcEdgeId = Edge_List.edges[i]->src_id;
            int dstEdgeId = Edge_List.edges[i]->dst_id;
            if (srcEdgeId == My_ID && edges[srcEdgeId][dstEdgeId] == 1)
            {
                if (update == 1)
                {
                    
                    // Alarm(PRINT, "got to sendlinkstate 1\n");
                    send_link_state(dstEdgeId, updateNum, 1, 0, 0, updates);
                }
                else
                {
                    
                    // Alarm(PRINT, "got to sendlinkstate 2\n");
                    send_link_state(dstEdgeId, updateNum, 0, messageSrc, messageUpdateNum, updates);
                }
            }
        }
        
        Alarm(PRINT, "hi\n");
        if (update == 1)
        {
            updateNum++;
            updates[My_ID] = updateNum;
        }
    }
}

void send_link_state(int neighbor_id, int update, int firstSend, int src, int messageUpdNum, int updates[MAX_NODES])
{
    struct lsa_pkt lsaPkt;
    struct node *neighbor;
    int bytes = 0;
    int ret = 0;
    lsaPkt.hdr.dst_id = neighbor_id;
    lsaPkt.hdr.src_id = My_ID;
    lsaPkt.hdr.type = CTRL_LSA;

    neighbor = get_node_from_id(&Node_List, neighbor_id);

    if (firstSend == 1) {
        lsaPkt.hdr.src_id = My_ID;
        lsaPkt.updateNum = update;
        for (int i = 0; i < MAX_NODES; i++)
        {
            lsaPkt.updateRow[i] = edges[My_ID][i];
            if (fwdingTable[i]== neighbor_id)
            {
                lsaPkt.updateRow[i] = edges[My_ID][i];
            }
        }
    } else {
        lsaPkt.hdr.src_id = src;
        lsaPkt.updateNum = messageUpdNum;
        for (int i = 0; i < MAX_NODES; i++)
        {
            lsaPkt.updateRow[i] = updates[i];
            if (fwdingTable[i] == neighbor_id)
            {
                lsaPkt.updateRow[i] = updates[i];
            }
        }
    }

    struct sockaddr_in ctrl_addr_ex = neighbor->ctrl_addr;
    ctrl_addr_ex.sin_port = htons(ntohs(neighbor->ctrl_addr.sin_port) + 1);

    bytes = sizeof(lsaPkt);
    ret = sendto(Ctrl_Sock, &lsaPkt,bytes, 0, (struct sockaddr *)&ctrl_addr_ex, sizeof(ctrl_addr_ex));
    if (ret < 0)
    {
        Alarm(PRINT, "Error sending link state advertisement to sock %d\n", Ctrl_Sock);
        goto err;
    }

    return;
err:
    remove_client_with_sock(&Client_List, Ctrl_Sock);
}

void init_distance_vector(void)
{
    Alarm(DEBUG, "init distance vector\n");
}

uint32_t ip_from_str(char *ip)
{
    struct in_addr addr;

    inet_pton(AF_INET, ip, &addr);
    return ntohl(addr.s_addr);
}

void process_conf(char *fname, int my_id)
{
    char buf[MAX_CONF_LINE];
    char ip_str[MAX_CONF_LINE];
    FILE *f = NULL;
    uint32_t id = 0;
    uint16_t port = 0;
    uint32_t src = 0;
    uint32_t dst = 0;
    uint32_t cost = 0;
    int node_sec_done = 0;
    int ret = -1;
    struct node n;
    struct edge e;
    struct node *retn = NULL;
    struct edge *rete = NULL;

    Alarm(DEBUG, "Processing configuration file %s\n", fname);

    /* Open configuration file */
    f = fopen(fname, "r");
    if (f == NULL)
    {
        Alarm(EXIT, "overlay_node: error: failed to open conf file %s : %s\n",
              fname, strerror(errno));
    }

    /* Read list of nodes from conf file */
    while (fgets(buf, MAX_CONF_LINE, f))
    {
        Alarm(DEBUG, "Read line: %s", buf);

        if (!node_sec_done)
        {
            // sscanf
            ret = sscanf(buf, "%u %s %hu", &id, ip_str, &port);
            Alarm(DEBUG, "    Node ID: %u, Node IP %s, Port: %u\n", id, ip_str, port);
            if (ret != 3)
            {
                Alarm(DEBUG, "done reading nodes\n");
                node_sec_done = 1;
                continue;
            }

            if (id == my_id)
            {
                Alarm(DEBUG, "Found my ID (%u). Setting IP and port\n", id);
                My_Port = port;
                My_IP = ip_from_str(ip_str);
            }

            n.id = id;
            n.next_hop = NULL;
            /* set up data address */
            memset(&n.data_addr, 0, sizeof(n.data_addr));
            n.data_addr.sin_family = AF_INET;
            n.data_addr.sin_addr.s_addr = htonl(ip_from_str(ip_str));
            n.data_addr.sin_port = htons(port);
            /* set up control address. note that we use port+1 for the control
             * port. */
            memset(&n.ctrl_addr, 0, sizeof(n.ctrl_addr));
            n.ctrl_addr.sin_family = AF_INET;
            n.ctrl_addr.sin_addr.s_addr = htonl(ip_from_str(ip_str));
            n.ctrl_addr.sin_port = htons(port + 1);

            /* add to list of nodes */
            retn = add_node_to_list(&Node_List, n);
            if (retn == NULL)
            {
                Alarm(EXIT, "Failed to add node to list\n");
            }
        }
        else
        { /* Edge section */
            ret = sscanf(buf, "%u %u %u", &src, &dst, &cost);
            Alarm(DEBUG, "    Src ID: %u, Dst ID %u, Cost: %u\n", src, dst, cost);
            if (ret != 3)
            {
                Alarm(DEBUG, "done reading nodes\n");
                node_sec_done = 1;
                continue;
            }

            e.src_id = src;
            e.dst_id = dst;
            e.cost = cost;
            e.src_node = get_node_from_id(&Node_List, e.src_id);
            e.dst_node = get_node_from_id(&Node_List, e.dst_id);
            if (e.src_node == NULL || e.dst_node == NULL)
            {
                Alarm(EXIT, "Failed to find node for edge (%u, %u)\n", src, dst);
            }
            rete = add_edge_to_list(&Edge_List, e);
            if (rete == NULL)
            {
                Alarm(EXIT, "Failed to add edge to list\n");
            }
        }
    }
}

int main(int argc, char **argv)
{

    char *conf_fname = NULL;

    if (PRINT_DEBUG)
    {
        Alarm_set_types(DEBUG);
    }

    /* parse args */
    if (argc != 4)
    {
        Alarm(EXIT, "usage: overlay_node <id> <config_file> <mode: LS/DV>\n");
    }

    My_ID = atoi(argv[1]);
    conf_fname = argv[2];

    if (!strncmp("LS", argv[3], 3))
    {
        Route_Mode = MODE_LINK_STATE;
    }
    else if (!strncmp("DV", argv[3], 3))
    {
        Route_Mode = MODE_DISTANCE_VECTOR;
    }
    else
    {
        Alarm(EXIT, "Invalid mode %s: should be LS or DV\n", argv[5]);
    }

    Alarm(DEBUG, "My ID             : %d\n", My_ID);
    Alarm(DEBUG, "Configuration file: %s\n", conf_fname);
    Alarm(DEBUG, "Mode              : %d\n\n", Route_Mode);

    process_conf(conf_fname, My_ID);
    Alarm(DEBUG, "My IP             : " IPF "\n", IP(My_IP));
    Alarm(DEBUG, "My Port           : %u\n", My_Port);

    { /* print node and edge lists from conf */
        int i;
        struct node *n;
        struct edge *e;
        for (i = 0; i < Node_List.num_nodes; i++)
        {
            n = Node_List.nodes[i];
            Alarm(DEBUG, "Node %u : data_addr: " IPF ":%u\n"
                         "         ctrl_addr: " IPF ":%u\n",
                  n->id,
                  IP(ntohl(n->data_addr.sin_addr.s_addr)),
                  ntohs(n->data_addr.sin_port),
                  IP(ntohl(n->ctrl_addr.sin_addr.s_addr)),
                  ntohs(n->ctrl_addr.sin_port));
        }

        for (i = 0; i < Edge_List.num_edges; i++)
        {
            e = Edge_List.edges[i];
            Alarm(DEBUG, "Edge (%u, %u) : " IPF ":%u -> " IPF ":%u\n",
                  e->src_id, e->dst_id,
                  IP(ntohl(e->src_node->data_addr.sin_addr.s_addr)),
                  ntohs(e->src_node->data_addr.sin_port),
                  IP(ntohl(e->dst_node->data_addr.sin_addr.s_addr)),
                  ntohs(e->dst_node->data_addr.sin_port));
        }
    }

    /* Initialize event system */
    E_init();

    /* Set up TCP socket for client connection requests */
    init_client_sock(My_Port);

    /* Set up UDP sockets for sending and receiving messages from other
     * overlay nodes */
    init_overlay_data_sock(My_Port);
    init_overlay_ctrl_sock(My_Port + 1);

    if (Route_Mode == MODE_LINK_STATE)
    {
        init_link_state();
    }
    else
    {
        init_distance_vector();
    }

    /* Enter event handling loop */
    Alarm(DEBUG, "Entering event loop!\n");
    E_handle_events();

    return 0;
}
