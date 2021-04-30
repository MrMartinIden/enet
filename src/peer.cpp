#include "enet.h"

ENetPacket *ENetPeer::receive(enet_uint8 *channelID)
{
    ENetIncomingCommand *incomingCommand;
    ENetPacket *packet;

    if (enet_list_empty(&this->dispatchedCommands))
    {
        return nullptr;
    }

    incomingCommand =
            (ENetIncomingCommand *)enet_list_remove(enet_list_begin(&this->dispatchedCommands));

    if (channelID != nullptr)
    {
        *channelID = incomingCommand->command.header.channelID;
    }

    packet = incomingCommand->packet;
    --packet->referenceCount;

    if (incomingCommand->fragments != nullptr)
    {
        enet_free(incomingCommand->fragments);
    }

    enet_free(incomingCommand);
    this->totalWaitingData -= packet->dataLength;

    return packet;
}

int ENetPeer::send(enet_uint8 channelID, ENetPacket *packet)
{
    ENetChannel *channel = &this->channels[channelID];
    ENetProtocol command;
    size_t fragmentLength;

    if (this->state != ENetPeerState::CONNECTED || channelID >= this->channelCount ||
            packet->dataLength > this->host->maximumPacketSize)
    {
        return -1;
    }

    fragmentLength = this->mtu - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment);
    if (this->host->checksum != nullptr)
    {
        fragmentLength -= sizeof(enet_uint32);
    }

    if (packet->dataLength > fragmentLength)
    {
        enet_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength, fragmentNumber, fragmentOffset;
        enet_uint8 commandNumber;
        enet_uint16 startSequenceNumber;
        ENetList fragments;
        ENetOutgoingCommand *fragment;

        if (fragmentCount > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
        {
            return -1;
        }

        if ((packet->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT)) ==
                        ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT &&
                channel->outgoingUnreliableSequenceNumber < 0xFFFF)
        {
            commandNumber = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
            startSequenceNumber = ENET_HOST_TO_NET_16(channel->outgoingUnreliableSequenceNumber + 1);
        }
        else
        {
            commandNumber = ENET_PROTOCOL_COMMAND_SEND_FRAGMENT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
            startSequenceNumber = ENET_HOST_TO_NET_16(channel->outgoingReliableSequenceNumber + 1);
        }

        enet_list_clear(&fragments);

        for (fragmentNumber = 0, fragmentOffset = 0; fragmentOffset < packet->dataLength; ++fragmentNumber, fragmentOffset += fragmentLength)
        {
            if (packet->dataLength - fragmentOffset < fragmentLength)
            {
                fragmentLength = packet->dataLength - fragmentOffset;
            }

            fragment = (ENetOutgoingCommand *)enet_malloc(sizeof(ENetOutgoingCommand));

            if (fragment == nullptr)
            {
                while (!enet_list_empty(&fragments))
                {
                    fragment = (ENetOutgoingCommand *)enet_list_remove(enet_list_begin(&fragments));

                    enet_free(fragment);
                }

                return -1;
            }

            fragment->fragmentOffset = fragmentOffset;
            fragment->fragmentLength = fragmentLength;
            fragment->packet = packet;
            fragment->command.header.command = commandNumber;
            fragment->command.header.channelID = channelID;

            fragment->command.sendFragment.startSequenceNumber = startSequenceNumber;

            fragment->command.sendFragment.dataLength = ENET_HOST_TO_NET_16(fragmentLength);
            fragment->command.sendFragment.fragmentCount = ENET_HOST_TO_NET_32(fragmentCount);
            fragment->command.sendFragment.fragmentNumber = ENET_HOST_TO_NET_32(fragmentNumber);
            fragment->command.sendFragment.totalLength = ENET_HOST_TO_NET_32(packet->dataLength);
            fragment->command.sendFragment.fragmentOffset = ENET_NET_TO_HOST_32(fragmentOffset);

            enet_list_insert(enet_list_end(&fragments), fragment);
        }

        packet->referenceCount += fragmentNumber;

        while (!enet_list_empty(&fragments))
        {
            fragment = (ENetOutgoingCommand *)enet_list_remove(enet_list_begin(&fragments));
            this->setup_outgoing_command(fragment);
        }

        return 0;
    }

    command.header.channelID = channelID;

    if ((packet->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED)) == ENET_PACKET_FLAG_UNSEQUENCED)
    {
        command.header.command = ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED | ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
        command.sendUnsequenced.dataLength = ENET_HOST_TO_NET_16(packet->dataLength);
    }
    else if (packet->flags & ENET_PACKET_FLAG_RELIABLE || channel->outgoingUnreliableSequenceNumber >= 0xFFFF)
    {
        command.header.command = ENET_PROTOCOL_COMMAND_SEND_RELIABLE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
        command.sendReliable.dataLength = ENET_HOST_TO_NET_16(packet->dataLength);
    }
    else
    {
        command.header.command = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE;
        command.sendUnreliable.dataLength = ENET_HOST_TO_NET_16(packet->dataLength);
    }

    if (this->queue_outgoing_command(&command, packet, 0, packet->dataLength) == nullptr)
    {
        return -1;
    }

    return 0;
} // enet_peer_send
