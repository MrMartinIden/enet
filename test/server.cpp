#include "enet.h"

void host_server(ENetHost& server)
{
    ENetEvent event;
    while (server.service(&event, 10) > 0) {
        switch (event.type) {
        case ENetEventType::CONNECT:
            printf("A new client connected from ::1:%u.\n", event.peer->address.port);

            /* Store any relevant client information here. */
            event.peer->data = reinterpret_cast<void*>(const_cast<char*>("Client0"));
            break;
        case ENetEventType::RECEIVE:
            printf("A packet of length %zu containing data = %s was received from %s on channel %u.\n",
                event.packet->dataLength,
                event.packet->data,
                reinterpret_cast<char*>(event.peer->get_data()),
                event.channelID);

            /* Clean up the packet now that we're done using it. */
            enet_packet_destroy(event.packet);
            break;

        case ENetEventType::DISCONNECT:
            printf("%s disconnected.\n", reinterpret_cast<char*>(event.peer->data));
            /* Reset the peer's client information. */
            event.peer->data = nullptr;
            break;

        case ENetEventType::DISCONNECT_TIMEOUT:
            printf("%s timeout.\n", reinterpret_cast<char*>(event.peer->data));
            event.peer->data = nullptr;
            break;

        case ENetEventType::NONE:
            break;
        }
    }
}

int main()
{
    if (enet_initialize() != 0) {
        printf("An error occurred while initializing ENet.\n");
        return 1;
    }

#define MAX_CLIENTS 32

    ENetAddress address = { 0 };

    address.host = ENET_HOST_ANY; /* Bind the server to the default localhost. */
    address.port = 7777; /* Bind the server to port 7777. */

    /* create a server */
    printf("starting server...\n");
    ENetHost server { &address, MAX_CLIENTS, 2, 0, 0 };

    do {
        host_server(server);

    } // while (counter > 0);
    while (true);

    //delete server;
    enet_deinitialize();
    return 0;
}
