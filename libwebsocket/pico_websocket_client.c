#include "pico_websocket_util.h"
#include "pico_websocket_client.h"
#include "../libhttp/pico_http_util.h"
#include <stdint.h>
#include "pico_tree.h"
#include "pico_config.h"
#include "pico_socket.h"
#include "pico_tcp.h"
#include "pico_ipv4.h"
#include "pico_stack.h"
#include "pico_socket.h"

//TODO: better macro or add client to macro
#define consumeChar(c)        (pico_socket_read(client->sck, &c, 1u))
#define WS_PROTO_TOK      "ws://"
#define WS_PROTO_LEN      5u

#define HTTP_HEADER_LINE_SIZE 50u
#define HTTP_RESPONSE_INDEX   9u
#define WEBSOCKET_COMMON_HEADER_SIZE 2u

#define PAYLOAD_LENGTH_SIZE_16_BIT_IN_BYTES 2u
#define PAYLOAD_LENGTH_SIZE_64_BIT_IN_BYTES 8u

#define MASKING_KEY_SIZE_IN_BYTES 4u

#define WS_UPGRADE_NOT_SENT   0u
#define WS_UPGRADE_SENT       1u
#define WS_READY_STATE        2u

static uint16_t GlobalWebSocketConnectionID= 0;

struct pico_websocket_client
{
        struct pico_socket *sck;
        void (*wakeup)(uint16_t ev, uint16_t conn);
        uint16_t connectionID;
        struct pico_websocket_header* hdr;
        uint32_t current_server_masking_key;
        struct pico_http_uri* uriKey;
        struct pico_ip4 ip;
        uint8_t state;
};

PACKED_STRUCT_DEF pico_websocket_header
{
        uint8_t opcode : 4;
        uint8_t RSV3 : 1;
        uint8_t RSV2 : 1;
        uint8_t RSV1 : 1;
        uint8_t fin : 1;
        uint8_t payload_length : 7; //TODO : read up + Multibyte length quantities are expressed in network byte order.
        uint8_t mask : 1;
};

static int compare_ws_with_connID(void *ka, void *kb)
{
        return ((struct pico_websocket_client *)ka)->connectionID - ((struct pico_websocket_client *)kb)->connectionID;
}

PICO_TREE_DECLARE(pico_websocket_client_list, compare_ws_with_connID);

static struct pico_websocket_client* retrieve_websocket_client_with_conn_ID(uint16_t wsConnID)
{
        struct pico_websocket_client dummy = {
                .connectionID = wsConnID
        };
        struct pico_websocket_client *client = pico_tree_findKey(&pico_websocket_client_list, &dummy);

        if(!client)
        {
                dbg("Wrong connection id !\n");
                pico_err = PICO_ERR_EINVAL;
                return NULL;
        }

        return client;

}

static struct pico_websocket_client* find_websocket_client_with_socket(struct pico_socket* s)
{
        struct pico_websocket_client *client = NULL;
        struct pico_tree_node *index;

        if (!s)
        {
                return NULL;
        }

        pico_tree_foreach(index, &pico_websocket_client_list)
        {
                if(((struct pico_websocket_client *)index->keyValue)->sck == s )
                {
                        client = (struct pico_websocket_client *)index->keyValue;
                        break;
                }
        }

        if(!client)
        {
                dbg("Client not found using given socket...Something went wrong !\n");
                return NULL;
        }

        return client;

}

//TODO: move to pico_http_util
static int8_t pico_process_URI(const char *uri, struct pico_http_uri *urikey)
{

        uint16_t lastIndex = 0, index;

        if(!uri || !urikey || uri[0] == '/')
        {
                pico_err = PICO_ERR_EINVAL;
                goto error;
        }

/* detect protocol => search for  "colon-slash-slash" */
        if(memcmp(uri, WS_PROTO_TOK, WS_PROTO_LEN) == 0) /* could be optimized */
        { /* protocol identified, it is ws */
                urikey->protoHttp = 1;
                lastIndex = WS_PROTO_LEN;
        }

/* detect hostname */
        index = lastIndex;
        while(uri[index] && uri[index] != '/' && uri[index] != ':') index++;
        if(index == lastIndex)
        {
/* wrong format */
                urikey->host = urikey->resource = NULL;
                urikey->port = urikey->protoHttp = 0u;
                pico_err = PICO_ERR_EINVAL;
                goto error;
        }
        else
        {
/* extract host */
                urikey->host = (char *)PICO_ZALLOC((uint32_t)(index - lastIndex + 1));

                if(!urikey->host)
                {
/* no memory */
                        pico_err = PICO_ERR_ENOMEM;
                        goto error;
                }

                memcpy(urikey->host, uri + lastIndex, (size_t)(index - lastIndex));
        }

        if(!uri[index])
        {
/* nothing specified */
                urikey->port = 80u;
                urikey->resource = PICO_ZALLOC(2u);
                if (!urikey->resource) {
/* no memory */
                        pico_err = PICO_ERR_ENOMEM;
                        goto error;
                }

                urikey->resource[0] = '/';
                return 0;
        }
        else if(uri[index] == '/')
        {
                urikey->port = 80u;
        }
        else if(uri[index] == ':')
        {
                urikey->port = 0u;
                index++;
                while(uri[index] && uri[index] != '/')
                {
/* should check if every component is a digit */
                        urikey->port = (uint16_t)(urikey->port * 10 + (uri[index] - '0'));
                        index++;
                }
        }

/* extract resource */
        if(!uri[index])
        {
                urikey->resource = PICO_ZALLOC(2u);
                if (!urikey->resource) {
/* no memory */
                        pico_err = PICO_ERR_ENOMEM;
                        goto error;
                }

                urikey->resource[0] = '/';
        }
        else
        {
                lastIndex = index;
                while(uri[index]) index++;
                urikey->resource = (char *)PICO_ZALLOC((size_t)(index - lastIndex + 1));

                if(!urikey->resource)
                {
/* no memory */
                        pico_err = PICO_ERR_ENOMEM;
                        goto error;
                }

                memcpy(urikey->resource, uri + lastIndex, (size_t)(index - lastIndex));
        }

        return 0;

error:
        if(urikey->resource)
        {
                PICO_FREE(urikey->resource);
                urikey->resource = NULL;
        }

        if(urikey->host)
        {
                PICO_FREE(urikey->host);
                urikey->host = NULL;
        }

        return -1;
}

static int pico_websocket_client_cleanup(struct pico_websocket_client* client)
{
        struct pico_websocket_client *toBeRemoved = NULL;
        int ret = -1;

        dbg("Closing the websocket client...\n");
        /*  TODO: sent close websocket message!*/

        toBeRemoved = pico_tree_delete(&pico_websocket_client_list, client);
        if(!toBeRemoved)
        {
                dbg("Warning ! Websocket to be closed not found ...");
                return -1;
        }

        if (client->sck)
        {
                pico_socket_close(client->sck);
        }
        return 0;
}

static int pico_websocket_mask_data(uint32_t masking_key, uint8_t* data, int size)
{
        int i;
        uint8_t mask[4];

        memcpy(mask, &masking_key, sizeof(uint32_t));

        for( i = 0 ; i < size ; ++i )
        {
                data[i] = data[i] ^ mask[i%4];
        }

        return i;
}

static uint32_t determine_masking_key_from_payload_length(uint8_t payload_length, struct pico_websocket_client* client){
        int ret;
        uint8_t* payload_length_buffer; /* TODO: store this for later if needed? */
        uint32_t masking_key;

        if (client == NULL)
        {
                goto error;
        }

        payload_length_buffer = PICO_ZALLOC(PAYLOAD_LENGTH_SIZE_64_BIT_IN_BYTES);

        if (!payload_length_buffer)
        {
                goto error;
        }

        switch (payload_length)
        {
        case WS_16_BIT_PAYLOAD_LENGTH_INDICATOR:
                ret = pico_socket_read(client->sck, payload_length_buffer, PAYLOAD_LENGTH_SIZE_16_BIT_IN_BYTES);
                break;
        case WS_64_BIT_PAYLOAD_LENGTH_INDICATOR:
                ret = pico_socket_read(client->sck, payload_length_buffer, PAYLOAD_LENGTH_SIZE_64_BIT_IN_BYTES);
                break;
        default:
                /* No extra reading required, size is already known */
                ret = payload_length;
                break;
        }

        if (ret < 0)
        {
                goto error;
        }

        /* Read masking key and store it */

        ret = pico_socket_read(client->sck, &masking_key, MASKING_KEY_SIZE_IN_BYTES);

        if (ret < 0)
        {
                goto error;
        }

        return masking_key;

error:
        if (payload_length_buffer)
        {
                PICO_FREE(payload_length_buffer);
        }
        return 0;

}

static void wakeup_client_using_opcode(uint8_t opcode, struct pico_websocket_client* client)
{
        switch(opcode)
        {
        case WS_CONTINUATION_FRAME:
                break;
        case WS_CONN_CLOSE:
                break;
        case WS_PING:
                break;
        case WS_PONG:
                break;
        case WS_BINARY_FRAME:
                break;
        case WS_TEXT_FRAME:
                /* Wakeup client, unmasking data will be handled in read api */
                client->wakeup(EV_WS_BODY, client->connectionID);
                break;
        default:
                /*bad message received*/
                client->wakeup(EV_WS_ERR, client->connectionID);
                break;
        }

}

static int parse_websocket_header(struct pico_websocket_client* client)
{
        /* TODO: change state of client when data is ready? */
        int ret;
        uint8_t* start_of_data;
        uint64_t payload_size;
        uint32_t masking_key;

        struct pico_websocket_header* hdr = client->hdr;

        /* FIN bit */

        if (hdr->fin)
        {
                /*This is the last packet in the message*/
        }
        else
        {
                /*This is part of a bigger message*/
        }

        /* Opcode */
        switch(hdr->opcode)
        {
        case WS_CONTINUATION_FRAME:
                ret = WS_CONTINUATION_FRAME;
                break;
        case WS_CONN_CLOSE:
                ret = WS_CONN_CLOSE;
                break;
        case WS_PING:
                ret = WS_PING;
                break;
        case WS_PONG:
                ret = WS_PONG;
                break;
        case WS_BINARY_FRAME:
                ret = WS_BINARY_FRAME;
                break;
        case WS_TEXT_FRAME:
                ret = WS_TEXT_FRAME;
                break;
        default:
                /*bad header*/
                return -1;
                break;
        }

        if (hdr->mask == WS_MASK_ENABLE)
        {
                /* Store the masking key for unmasking when client calls readdata */
                client->current_server_masking_key = determine_masking_key_from_payload_length(hdr->payload_length, client);
                if (client->current_server_masking_key == 0 )
                {
                        dbg("Masking key could not be determined.\n");
                        return -1;
                }
        }
        else
        {
                client->current_server_masking_key = 0;
        }

        return ret;
}

static void handle_websocket_message(struct pico_websocket_client* client)
{
        int ret;

        ret = pico_socket_read(client->sck, client->hdr , WEBSOCKET_COMMON_HEADER_SIZE);
        if (ret < 0)
        {
                client->wakeup(EV_WS_ERR, client->connectionID);
                return;
        }

        ret = parse_websocket_header(client);

        wakeup_client_using_opcode(ret, client);
}

static char* pico_websocket_upgradeHeader_build(void)
{
        char* header;
        int header_size = 256;

        header = PICO_ZALLOC(header_size);
        strcpy(header, "GET /chat HTTP/1.1\r\n");
        strcat(header, "Host: 10.50.0.1:8888\r\n"); // TODO: dynamic
        strcat(header, "Upgrade: websocket\r\n");
        strcat(header, "Connection: Upgrade\r\n");
        strcat(header, "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n"); // this is a 16-byte value that has been base64-encoded, it is randomly selected (pico_rand?).
        strcat(header, "Sec-WebSocket-Protocol: chat, superchat\r\n");
        strcat(header, "Sec-WebSocket-Version: 13\r\n");
        strcat(header, "Origin: http://example.com\r\n");
        strcat(header, "\r\n");
        return header;
}

static struct pico_websocket_header* pico_websocket_client_build_header(int dataSize)
{
        struct pico_websocket_header * header = PICO_ZALLOC(sizeof(struct pico_websocket_header));

        if (!header)
        {
                dbg("Could not allocate websocket header.\n");
                return NULL;
        }


        header->fin = WS_FIN_ENABLE; // TODO: dependent of size
        header->RSV1 = 0; // TODO: define WS_USR_RSV1/2/3?
        header->RSV2 = 0;
        header->RSV3 = 0;
        header->opcode = WS_TEXT_FRAME;
        header->mask = WS_MASK_ENABLE;

        header->payload_length = dataSize; // TODO: read up on payload length + implement further

        return header;
}

static int pico_websocket_client_send_upgrade_header(struct pico_socket* socket)
{
        char *header;
        int ret;

        header= pico_websocket_upgradeHeader_build();
        if (!header)
        {
                dbg("WebSocket Header could not be created.\n");
                return -1;
        }

        ret = pico_socket_write(socket, header, strlen(header));

        if(ret < 0)
        {
                dbg("Failed to send upgrade header.\n");
                return -1;
        }

        return ret;
}

static inline void ws_read_first_line(struct pico_websocket_client* client, char *line, uint32_t *index)
{
        char c;

        /* read the first line of the header */
        while(consumeChar(c) > 0 && c != '\r')
        {
                if(*index < HTTP_HEADER_LINE_SIZE) /* truncate if too long */
                        line[(*index)++] = c;
        }
        consumeChar(c); /* consume \n */
}

static int ws_parse_upgrade_header(struct pico_websocket_client* client)
{
        char line[HTTP_HEADER_LINE_SIZE];
        uint32_t index = 0;
        uint16_t responseCode = -1;
        char c;

        ws_read_first_line(client, line, &index);

        responseCode = (uint16_t)((line[HTTP_RESPONSE_INDEX] - '0') * 100 +
                                  (line[HTTP_RESPONSE_INDEX + 1] - '0') * 10 +
                                  (line[HTTP_RESPONSE_INDEX + 2] - '0'));
        if (responseCode != HTTP_SWITCHING_PROTOCOLS) {
                dbg("Response to upgrade request was not HTTP_SWITCHING_PROTOCOLS (101).\n");
                return -1;
        }

        /* Response was OK, consume the rest of it to ensure full frame is discarded */
        while(consumeChar(c) > 0)
        {

        }


        return 0;
}

static void ws_treat_read_event(struct pico_websocket_client* client)
{
        dbg("treat read event, client state: %d\n", client->state);
        if(client->state != WS_UPGRADE_SENT && client->state != WS_READY_STATE)
        {
                dbg("Upgrade not sent but tried to treat read event.\n");
                return;
        }

        if(client->state == WS_UPGRADE_SENT && client->state != WS_READY_STATE)
        {
                dbg("Parse upgrade header sent by back server.\n");
                client->hdr = PICO_ZALLOC(sizeof(struct pico_websocket_header));

                if(!client->hdr)
                {
                        dbg("Out of memory to allocate websocket header.\n");
                        pico_err = PICO_ERR_ENOMEM;
                        return;
                }
                int ret = ws_parse_upgrade_header(client);

                if (ret < 0)
                {
                        dbg("received bad header from server.\n");
                        return;
                }

                client->state = WS_READY_STATE;
        }

        else
        {
                handle_websocket_message(client);
        }
}


static void ws_tcp_callback(uint16_t ev, struct pico_socket *s)
{
        struct pico_websocket_client *client;

        client = find_websocket_client_with_socket(s);

        if(!client)
        {
                dbg("Client not found...Something went wrong !\n");
                return;
        }


        if(ev & PICO_SOCK_EV_CONN)
        {
                int a = pico_websocket_client_send_upgrade_header(s);
                if (a < 0)
                {
                        dbg("Webserver client sendHeader failed.\n");
                        pico_websocket_client_close(client->connectionID);
                        return;
                }
                client->state= WS_UPGRADE_SENT;
        }

        if(ev & PICO_SOCK_EV_ERR)
        {
                dbg("Error happened with socket.\n");
                pico_websocket_client_close(client->connectionID);
        }

        if((ev & PICO_SOCK_EV_CLOSE) || (ev & PICO_SOCK_EV_FIN))
        {
                dbg("Close/FIN request received.\n");
                pico_websocket_client_close(client->connectionID);
        }

        if(ev & PICO_SOCK_EV_RD)
        {
                ws_treat_read_event(client);
        }

        if(ev & PICO_SOCK_EV_WR)
        {

        }
}



/* used for getting a response from DNS servers */
static void dnsCallback(char *ip, void *ptr)
{
        struct pico_websocket_client *client = (struct pico_websocket_client *)ptr;

        if(!client)
        {
                dbg("Who made the request ?!\n");
                return;
        }

        if(ip)
        {
                pico_string_to_ipv4(ip, &client->ip.addr);
                client->sck = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_TCP, &ws_tcp_callback);
                if(!client->sck)
                {
                        dbg("Failed to open socket.\n");
                        client->wakeup(EV_WS_ERR, client->connectionID);
                        return;
                }

                if(pico_socket_connect(client->sck, &client->ip, short_be(client->uriKey->port)) < 0)
                {
                        client->wakeup(EV_WS_ERR, client->connectionID);
                        return;
                }

        }
        else
        {
                /* wakeup client and let know error occured */
                client->wakeup(EV_WS_ERR, client->connectionID);

                /* close the client (free used heap) */
                pico_websocket_client_close(client->connectionID);
        }
}


/*
 * API for opening a new websocket client
 *
 * returns the connection ID of the websocket client
 */
int pico_websocket_client_open(char *uri, void (*wakeup)(uint16_t ev, uint16_t conn))
{
        struct pico_websocket_client *client;
        uint32_t ip = 0;

        client = PICO_ZALLOC(sizeof(struct pico_websocket_client));
        if (!client)
        {
                dbg("Could not allocate websocket client.\n");
                return -1;
        }

        client->wakeup = wakeup;
        client->connectionID = GlobalWebSocketConnectionID++;
        client->state = WS_UPGRADE_NOT_SENT;

        client->uriKey = PICO_ZALLOC(sizeof(struct pico_http_uri));

        if(!client->uriKey)
        {
                dbg("Failed to allocate urikey for websocket client.\n");
                return -1;
        }

        pico_process_URI(uri, client->uriKey);

        /* dns query */
        if(pico_string_to_ipv4(client->uriKey->host, &ip) == -1)
        {
                dbg("Querying : %s \n", client->uriKey->host);
                pico_dns_client_getaddr(client->uriKey->host, dnsCallback, client);
        }
        else
        {
                dbg("host already and ip address, no dns required.\n");
                dnsCallback(client->uriKey->host, client);
        }


        if(pico_tree_insert(&pico_websocket_client_list, client))
        {
                /* already in : should not be possible*/
                pico_err = PICO_ERR_EEXIST;
                //TODO: cleanup!
                return -1;
        }

        return client->connectionID;
}

/*
 * API for closing a websocket client
 * The argument connID can be a http client connection ID associated with a websocket client or
 * a websocket client connection ID.
 * returns 0 on success, < 0 if something went wrong
 */
int pico_websocket_client_close(uint16_t connID)
{
        struct pico_websocket_client *client;

        client = retrieve_websocket_client_with_conn_ID(connID);

        if (!client)
        {
                dbg("Websocket client cannot be closed, wrong connID provided!");
                return -1;
        }

        return pico_websocket_client_cleanup(client);
}

/*
 * API for reading data sent by the websocket server
 *
 *
 */
void pico_websocket_client_readData(uint16_t conn, void* data, uint16_t size)
{
        int ret;
        struct pico_websocket_client* client = retrieve_websocket_client_with_conn_ID(conn);
        if (!client)
        {
                dbg("Wrong conn ID for readData!\n");
                return;
        }

        /* TODO: how to handle multiple frames (FIN bit not set for some messages)? */

        ret = pico_socket_read(client->sck, data, size);




        return ret;
}

/*
 * API for sending data to the websocket server
 *
 *
 */
int pico_websocket_client_writeData(uint16_t conn, void* data, uint16_t size)
{
        int ret;
        struct pico_websocket_client* client = retrieve_websocket_client_with_conn_ID(conn);
        struct pico_socket* socket = client->sck;
        struct pico_websocket_header* header = pico_websocket_client_build_header(size);

        uint32_t masking_key = pico_rand();

        ret = pico_websocket_mask_data(masking_key, (uint8_t*)data, size);

        if (ret != size)
        {
                dbg("Error masking data.\n");
                return -1;
        }

        //TODO: if not all data can be written immediately, keep writing until all data is written.
        ret = pico_socket_write(socket, header, sizeof(struct pico_websocket_header));
        ret = pico_socket_write(socket, &masking_key, sizeof(uint32_t));
        ret = pico_socket_write(socket, data, size);

        return ret;
}
