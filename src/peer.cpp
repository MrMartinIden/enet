#include "enet.h"

enet_uint32 ENetPeer::get_id()
{
    return this->connectID;
}

enet_uint32 ENetPeer::get_ip(char *ip, size_t ipLength)
{
    return enet_address_get_host_ip(&this->address, ip, ipLength);
}

enet_uint16 ENetPeer::get_port()
{
    return this->address.port;
}

ENetPeerState ENetPeer::get_state()
{
    return this->state;
}

enet_uint32 ENetPeer::get_rtt()
{
    return this->roundTripTime;
}

enet_uint64 ENetPeer::get_packets_sent()
{
    return this->totalPacketsSent;
}

enet_uint32 ENetPeer::get_packets_lost()
{
    return this->totalPacketsLost;
}

enet_uint64 ENetPeer::get_bytes_sent()
{
    return this->totalDataSent;
}

enet_uint64 ENetPeer::get_bytes_received()
{
    return this->totalDataReceived;
}

void *ENetPeer::get_data()
{
    return (void *)this->data;
}

void ENetPeer::set_data(const void *data)
{
    this->data = (enet_uint32 *)data;
}

void ENetPeer::on_connect()
{
    if (this->state != ENetPeerState::CONNECTED &&
            this->state != ENetPeerState::DISCONNECT_LATER)
    {
        if (this->incomingBandwidth != 0)
        {
            ++this->host->bandwidthLimitedPeers;
        }

        ++this->host->connectedPeers;
    }
}

void ENetPeer::on_disconnect()
{
    if (this->state == ENetPeerState::CONNECTED ||
            this->state == ENetPeerState::DISCONNECT_LATER)
    {
        if (this->incomingBandwidth != 0)
        {
            --this->host->bandwidthLimitedPeers;
        }

        --this->host->connectedPeers;
    }
}

void ENetPeer::dispatch_incoming_unreliable_commands(ENetChannel *channel)
{
    ENetListIterator droppedCommand, startCommand, currentCommand;

    for (droppedCommand = startCommand = currentCommand = enet_list_begin(&channel->incomingUnreliableCommands);
            currentCommand != enet_list_end(&channel->incomingUnreliableCommands);
            currentCommand = enet_list_next(currentCommand))
    {
        ENetIncomingCommand *incomingCommand = (ENetIncomingCommand *)currentCommand;

        if ((incomingCommand->command.header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
        {
            continue;
        }

        if (incomingCommand->reliableSequenceNumber == channel->incomingReliableSequenceNumber)
        {
            if (incomingCommand->fragmentsRemaining <= 0)
            {
                channel->incomingUnreliableSequenceNumber = incomingCommand->unreliableSequenceNumber;
                continue;
            }

            if (startCommand != currentCommand)
            {
                enet_list_move(enet_list_end(&this->dispatchedCommands), startCommand,
                        enet_list_previous(currentCommand));

                if (!this->needsDispatch)
                {
                    host->dispatchQueue.push_back(this);

                    this->needsDispatch = 1;
                }

                droppedCommand = currentCommand;
            }
            else if (droppedCommand != currentCommand)
            {
                droppedCommand = enet_list_previous(currentCommand);
            }
        }
        else
        {
            enet_uint16 reliableWindow = incomingCommand->reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
            enet_uint16 currentWindow = channel->incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

            if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
            {
                reliableWindow += ENET_PEER_RELIABLE_WINDOWS;
            }

            if (reliableWindow >= currentWindow && reliableWindow < currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
            {
                break;
            }

            droppedCommand = enet_list_next(currentCommand);

            if (startCommand != currentCommand)
            {
                enet_list_move(enet_list_end(&this->dispatchedCommands), startCommand,
                        enet_list_previous(currentCommand));

                if (!this->needsDispatch)
                {
                    host->dispatchQueue.push_back(this);

                    this->needsDispatch = 1;
                }
            }
        }

        startCommand = enet_list_next(currentCommand);
    }

    if (startCommand != currentCommand)
    {
        enet_list_move(enet_list_end(&this->dispatchedCommands), startCommand,
                enet_list_previous(currentCommand));

        if (!this->needsDispatch)
        {
            host->dispatchQueue.push_back(this);
            this->needsDispatch = 1;
        }

        droppedCommand = currentCommand;
    }

    enet_peer_remove_incoming_commands(&channel->incomingUnreliableCommands, enet_list_begin(&channel->incomingUnreliableCommands), droppedCommand);
}

void ENetPeer::dispatch_incoming_reliable_commands(ENetChannel *channel)
{
    ENetListIterator currentCommand;

    for (currentCommand = enet_list_begin(&channel->incomingReliableCommands);
            currentCommand != enet_list_end(&channel->incomingReliableCommands);
            currentCommand = enet_list_next(currentCommand))
    {
        ENetIncomingCommand *incomingCommand = (ENetIncomingCommand *)currentCommand;

        if (incomingCommand->fragmentsRemaining > 0 || incomingCommand->reliableSequenceNumber != (enet_uint16)(channel->incomingReliableSequenceNumber + 1))
        {
            break;
        }

        channel->incomingReliableSequenceNumber = incomingCommand->reliableSequenceNumber;

        if (incomingCommand->fragmentCount > 0)
        {
            channel->incomingReliableSequenceNumber += incomingCommand->fragmentCount - 1;
        }
    }

    if (currentCommand == enet_list_begin(&channel->incomingReliableCommands))
    {
        return;
    }

    channel->incomingUnreliableSequenceNumber = 0;
    enet_list_move(enet_list_end(&this->dispatchedCommands),
            enet_list_begin(&channel->incomingReliableCommands),
            enet_list_previous(currentCommand));

    if (!this->needsDispatch)
    {
        host->dispatchQueue.push_back(this);
        this->needsDispatch = 1;
    }

    if (!enet_list_empty(&channel->incomingUnreliableCommands))
    {
        this->dispatch_incoming_unreliable_commands(channel);
    }
}

void ENetPeer::queue_acknowledgement(const ENetProtocol *command, enet_uint16 sentTime)
{
    if (command->header.channelID < this->channelCount)
    {
        ENetChannel *channel = &this->channels[command->header.channelID];
        enet_uint16 reliableWindow = command->header.reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
        [[maybe_unused]] enet_uint16 currentWindow = channel->incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

        if (command->header.reliableSequenceNumber < channel->incomingReliableSequenceNumber)
        {
            reliableWindow += ENET_PEER_RELIABLE_WINDOWS;
        }
    }

    this->outgoingDataTotal += sizeof(ENetProtocolAcknowledge);

    ENetAcknowledgement acknowledgement = { *command, sentTime };

    this->acknowledgements.insert(this->acknowledgements.end(), acknowledgement);
}

ENetIncomingCommand *ENetPeer::queue_incoming_command(const ENetProtocol *command,
        const void *data,
        size_t dataLength,
        enet_uint32 flags,
        enet_uint32 fragmentCount)
{
    static ENetIncomingCommand dummyCommand;

    ENetChannel *channel = &this->channels[command->header.channelID];
    enet_uint32 unreliableSequenceNumber = 0, reliableSequenceNumber = 0;
    enet_uint16 reliableWindow, currentWindow;
    ENetIncomingCommand *incomingCommand;
    ENetListIterator currentCommand;
    ENetPacket *packet = nullptr;

    auto destroy_if = [](ENetPacket *packet) -> void {
        if (packet != nullptr && packet->referenceCount == 0)
        {
            enet_packet_destroy(packet);
        }
    };

    auto notifyError = [destroy_if](ENetPacket *packet) -> ENetIncomingCommand * {
        destroy_if(packet);

        return nullptr;
    };

    auto discardCommand = [destroy_if,
                                  notifyError](ENetPacket *packet,
                                  enet_uint32 fragmentCount) -> ENetIncomingCommand * {
        if (fragmentCount > 0)
        {
            return notifyError(packet);
        }

        destroy_if(packet);

        return &dummyCommand;
    };

    if (this->state == ENetPeerState::DISCONNECT_LATER)
    {
        return discardCommand(packet, fragmentCount);
    }

    if ((command->header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
    {
        reliableSequenceNumber = command->header.reliableSequenceNumber;
        reliableWindow = reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
        currentWindow = channel->incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

        if (reliableSequenceNumber < channel->incomingReliableSequenceNumber)
        {
            reliableWindow += ENET_PEER_RELIABLE_WINDOWS;
        }

        if (reliableWindow < currentWindow || reliableWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
        {
            return discardCommand(packet, fragmentCount);
        }
    }

    switch (command->header.command & ENET_PROTOCOL_COMMAND_MASK)
    {
        case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
        case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
            if (reliableSequenceNumber == channel->incomingReliableSequenceNumber)
            {
                return discardCommand(packet, fragmentCount);
            }

            for (currentCommand = enet_list_previous(enet_list_end(&channel->incomingReliableCommands));
                    currentCommand != enet_list_end(&channel->incomingReliableCommands);
                    currentCommand = enet_list_previous(currentCommand))
            {
                incomingCommand = (ENetIncomingCommand *)currentCommand;

                if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
                {
                    if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
                    {
                        continue;
                    }
                }
                else if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
                {
                    break;
                }

                if (incomingCommand->reliableSequenceNumber <= reliableSequenceNumber)
                {
                    if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
                    {
                        break;
                    }

                    return discardCommand(packet, fragmentCount);
                }
            }
            break;

        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
            unreliableSequenceNumber = ENET_NET_TO_HOST_16(command->sendUnreliable.unreliableSequenceNumber);

            if (reliableSequenceNumber == channel->incomingReliableSequenceNumber && unreliableSequenceNumber <= channel->incomingUnreliableSequenceNumber)
            {
                return discardCommand(packet, fragmentCount);
            }

            for (currentCommand = enet_list_previous(enet_list_end(&channel->incomingUnreliableCommands));
                    currentCommand != enet_list_end(&channel->incomingUnreliableCommands);
                    currentCommand = enet_list_previous(currentCommand))
            {
                incomingCommand = (ENetIncomingCommand *)currentCommand;

                if ((command->header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
                {
                    continue;
                }

                if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
                {
                    if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
                    {
                        continue;
                    }
                }
                else if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
                {
                    break;
                }

                if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
                {
                    break;
                }

                if (incomingCommand->reliableSequenceNumber > reliableSequenceNumber)
                {
                    continue;
                }

                if (incomingCommand->unreliableSequenceNumber <= unreliableSequenceNumber)
                {
                    if (incomingCommand->unreliableSequenceNumber < unreliableSequenceNumber)
                    {
                        break;
                    }

                    return discardCommand(packet, fragmentCount);
                }
            }
            break;

        case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
            currentCommand = enet_list_end(&channel->incomingUnreliableCommands);
            break;

        default:
            return discardCommand(packet, fragmentCount);
    }

    if (this->totalWaitingData >= this->host->maximumWaitingData)
    {
        return notifyError(packet);
    }

    packet = enet_packet_create(data, dataLength, flags);
    if (packet == nullptr)
    {
        return notifyError(packet);
    }

    incomingCommand = (ENetIncomingCommand *)enet_malloc(sizeof(ENetIncomingCommand));
    if (incomingCommand == nullptr)
    {
        return notifyError(packet);
    }

    incomingCommand->reliableSequenceNumber = command->header.reliableSequenceNumber;
    incomingCommand->unreliableSequenceNumber = unreliableSequenceNumber & 0xFFFF;
    incomingCommand->command = *command;
    incomingCommand->fragmentCount = fragmentCount;
    incomingCommand->fragmentsRemaining = fragmentCount;
    incomingCommand->packet = packet;
    incomingCommand->fragments = nullptr;

    if (fragmentCount > 0)
    {
        if (fragmentCount <= ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
        {
            incomingCommand->fragments = (enet_uint32 *)enet_malloc((fragmentCount + 31) / 32 * sizeof(enet_uint32));
        }

        if (incomingCommand->fragments == nullptr)
        {
            enet_free(incomingCommand);

            return notifyError(packet);
        }

        memset(incomingCommand->fragments, 0, (fragmentCount + 31) / 32 * sizeof(enet_uint32));
    }

    if (packet != nullptr)
    {
        ++packet->referenceCount;
        this->totalWaitingData += packet->dataLength;
    }

    enet_list_insert(enet_list_next(currentCommand), incomingCommand);

    switch (command->header.command & ENET_PROTOCOL_COMMAND_MASK)
    {
        case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
        case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
            this->dispatch_incoming_reliable_commands(channel);
            break;

        default:
            this->dispatch_incoming_unreliable_commands(channel);
            break;
    }

    return incomingCommand;

} /* enet_peer_queue_incoming_command */

ENetOutgoingCommand *ENetPeer::queue_outgoing_command(const ENetProtocol *command,
        ENetPacket *packet,
        enet_uint32 offset,
        enet_uint16 length)
{
    ENetOutgoingCommand *outgoingCommand = (ENetOutgoingCommand *)enet_malloc(sizeof(ENetOutgoingCommand));

    if (outgoingCommand == nullptr)
    {
        return nullptr;
    }

    outgoingCommand->command = *command;
    outgoingCommand->fragmentOffset = offset;
    outgoingCommand->fragmentLength = length;
    outgoingCommand->packet = packet;
    if (packet != nullptr)
    {
        ++packet->referenceCount;
    }

    this->setup_outgoing_command(outgoingCommand);
    return outgoingCommand;
}

void ENetPeer::setup_outgoing_command(ENetOutgoingCommand *outgoingCommand)
{
    ENetChannel *channel = &this->channels[outgoingCommand->command.header.channelID];
    this->outgoingDataTotal +=
            enet_protocol_command_size(outgoingCommand->command.header.command) +
            outgoingCommand->fragmentLength;

    if (outgoingCommand->command.header.channelID == 0xFF)
    {
        ++this->outgoingReliableSequenceNumber;

        outgoingCommand->reliableSequenceNumber = this->outgoingReliableSequenceNumber;
        outgoingCommand->unreliableSequenceNumber = 0;
    }
    else if (outgoingCommand->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
    {
        ++channel->outgoingReliableSequenceNumber;
        channel->outgoingUnreliableSequenceNumber = 0;

        outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
        outgoingCommand->unreliableSequenceNumber = 0;
    }
    else if (outgoingCommand->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED)
    {
        ++this->outgoingUnsequencedGroup;

        outgoingCommand->reliableSequenceNumber = 0;
        outgoingCommand->unreliableSequenceNumber = 0;
    }
    else
    {
        if (outgoingCommand->fragmentOffset == 0)
        {
            ++channel->outgoingUnreliableSequenceNumber;
        }

        outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
        outgoingCommand->unreliableSequenceNumber = channel->outgoingUnreliableSequenceNumber;
    }

    outgoingCommand->sendAttempts = 0;
    outgoingCommand->sentTime = 0;
    outgoingCommand->roundTripTimeout = 0;
    outgoingCommand->roundTripTimeoutLimit = 0;
    outgoingCommand->command.header.reliableSequenceNumber = ENET_HOST_TO_NET_16(outgoingCommand->reliableSequenceNumber);

    switch (outgoingCommand->command.header.command & ENET_PROTOCOL_COMMAND_MASK)
    {
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
            outgoingCommand->command.sendUnreliable.unreliableSequenceNumber = ENET_HOST_TO_NET_16(outgoingCommand->unreliableSequenceNumber);
            break;

        case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
            outgoingCommand->command.sendUnsequenced.unsequencedGroup =
                    ENET_HOST_TO_NET_16(this->outgoingUnsequencedGroup);
            break;

        default:
            break;
    }

    if (outgoingCommand->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
    {
        enet_list_insert(enet_list_end(&this->outgoingReliableCommands), outgoingCommand);
    }
    else
    {
        enet_list_insert(enet_list_end(&this->outgoingUnreliableCommands), outgoingCommand);
    }
}

void ENetPeer::reset_queues()
{

    if (this->needsDispatch)
    {
        this->needsDispatch = 0;
    }

    while (!this->acknowledgements.empty())
    {
        this->acknowledgements.pop_front();
    }

    enet_peer_reset_outgoing_commands(&this->sentReliableCommands);
    enet_peer_reset_outgoing_commands(this->sentUnreliableCommands);
    enet_peer_reset_outgoing_commands(&this->outgoingReliableCommands);
    enet_peer_reset_outgoing_commands(&this->outgoingUnreliableCommands);
    enet_peer_reset_incoming_commands(&this->dispatchedCommands);

    if (this->channels != nullptr && this->channelCount > 0)
    {
        for (auto channel = this->channels; channel < &this->channels[this->channelCount];
                ++channel)
        {
            enet_peer_reset_incoming_commands(&channel->incomingReliableCommands);
            enet_peer_reset_incoming_commands(&channel->incomingUnreliableCommands);
        }

        enet_free(this->channels);
    }

    this->channels = nullptr;
    this->channelCount = 0;
}

void ENetPeer::throttle_configure(enet_uint32 interval, enet_uint32 acceleration,
        enet_uint32 deceleration)
{
    ENetProtocol command;

    this->packetThrottleInterval = interval;
    this->packetThrottleAcceleration = acceleration;
    this->packetThrottleDeceleration = deceleration;

    command.header.command = ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.header.channelID = 0xFF;

    command.throttleConfigure.packetThrottleInterval = ENET_HOST_TO_NET_32(interval);
    command.throttleConfigure.packetThrottleAcceleration = ENET_HOST_TO_NET_32(acceleration);
    command.throttleConfigure.packetThrottleDeceleration = ENET_HOST_TO_NET_32(deceleration);

    this->queue_outgoing_command(&command, nullptr, 0, 0);
}

void ENetPeer::disconnect_now(enet_uint32 data)
{
    ENetProtocol command;

    if (this->state == ENetPeerState::DISCONNECTED)
    {
        return;
    }

    if (this->state != ENetPeerState::ZOMBIE && this->state != ENetPeerState::DISCONNECTING)
    {
        this->reset_queues();

        command.header.command = ENET_PROTOCOL_COMMAND_DISCONNECT | ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
        command.header.channelID = 0xFF;
        command.disconnect.data = ENET_HOST_TO_NET_32(data);

        this->queue_outgoing_command(&command, nullptr, 0, 0);
        this->host->flush();
    }

    this->reset();
}

void ENetPeer::disconnect(enet_uint32 data)
{
    ENetProtocol command;

    if (this->state == ENetPeerState::DISCONNECTING ||
            this->state == ENetPeerState::DISCONNECTED ||
            this->state == ENetPeerState::ACKNOWLEDGING_DISCONNECT ||
            this->state == ENetPeerState::ZOMBIE)
    {
        return;
    }

    this->reset_queues();

    command.header.command = ENET_PROTOCOL_COMMAND_DISCONNECT;
    command.header.channelID = 0xFF;
    command.disconnect.data = ENET_HOST_TO_NET_32(data);

    if (this->state == ENetPeerState::CONNECTED ||
            this->state == ENetPeerState::DISCONNECT_LATER)
    {
        command.header.command |= ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    }
    else
    {
        command.header.command |= ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
    }

    this->queue_outgoing_command(&command, nullptr, 0, 0);

    if (this->state == ENetPeerState::CONNECTED ||
            this->state == ENetPeerState::DISCONNECT_LATER)
    {
        this->on_disconnect();

        this->state = ENetPeerState::DISCONNECTING;
    }
    else
    {
        this->host->flush();
        this->reset();
    }
}

void ENetPeer::disconnect_later(enet_uint32 data)
{
    if ((this->state == ENetPeerState::CONNECTED ||
                this->state == ENetPeerState::DISCONNECT_LATER) &&
            !(enet_list_empty(&this->outgoingReliableCommands) &&
                    enet_list_empty(&this->outgoingUnreliableCommands) &&
                    enet_list_empty(&this->sentReliableCommands)))
    {
        this->state = ENetPeerState::DISCONNECT_LATER;
        this->eventData = data;
    }
    else
    {
        this->disconnect(data);
    }
}

void ENetPeer::reset()
{
    this->on_disconnect();

    // We don't want to reset connectID here, otherwise, we can't get it in the Disconnect event
    // peer->connectID                     = 0;
    this->outgoingPeerID = ENET_PROTOCOL_MAXIMUM_PEER_ID;
    this->state = ENetPeerState::DISCONNECTED;
    this->incomingBandwidth = 0;
    this->outgoingBandwidth = 0;
    this->incomingBandwidthThrottleEpoch = 0;
    this->outgoingBandwidthThrottleEpoch = 0;
    this->incomingDataTotal = 0;
    this->totalDataReceived = 0;
    this->outgoingDataTotal = 0;
    this->totalDataSent = 0;
    this->lastSendTime = 0;
    this->lastReceiveTime = 0;
    this->nextTimeout = 0;
    this->earliestTimeout = 0;
    this->packetLossEpoch = 0;
    this->packetsSent = 0;
    this->totalPacketsSent = 0;
    this->packetsLost = 0;
    this->totalPacketsLost = 0;
    this->packetLoss = 0;
    this->packetLossVariance = 0;
    this->packetThrottle = ENET_PEER_DEFAULT_PACKET_THROTTLE;
    this->packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
    this->packetThrottleCounter = 0;
    this->packetThrottleEpoch = 0;
    this->packetThrottleAcceleration = ENET_PEER_PACKET_THROTTLE_ACCELERATION;
    this->packetThrottleDeceleration = ENET_PEER_PACKET_THROTTLE_DECELERATION;
    this->packetThrottleInterval = ENET_PEER_PACKET_THROTTLE_INTERVAL;
    this->pingInterval = ENET_PEER_PING_INTERVAL;
    this->timeoutLimit = ENET_PEER_TIMEOUT_LIMIT;
    this->timeoutMinimum = ENET_PEER_TIMEOUT_MINIMUM;
    this->timeoutMaximum = ENET_PEER_TIMEOUT_MAXIMUM;
    this->lastRoundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
    this->lowestRoundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
    this->lastRoundTripTimeVariance = 0;
    this->highestRoundTripTimeVariance = 0;
    this->roundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
    this->roundTripTimeVariance = 0;
    this->mtu = this->host->mtu;
    this->reliableDataInTransit = 0;
    this->outgoingReliableSequenceNumber = 0;
    this->windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    this->incomingUnsequencedGroup = 0;
    this->outgoingUnsequencedGroup = 0;
    this->eventData = 0;
    this->totalWaitingData = 0;

    this->unsequencedWindow = { 0 };
    this->reset_queues();
}

void ENetPeer::timeout(enet_uint32 timeoutLimit, enet_uint32 timeoutMinimum,
        enet_uint32 timeoutMaximum)
{
    this->timeoutLimit = timeoutLimit ? timeoutLimit : ENET_PEER_TIMEOUT_LIMIT;
    this->timeoutMinimum = timeoutMinimum ? timeoutMinimum : ENET_PEER_TIMEOUT_MINIMUM;
    this->timeoutMaximum = timeoutMaximum ? timeoutMaximum : ENET_PEER_TIMEOUT_MAXIMUM;
}

void ENetPeer::ping_interval(enet_uint32 pingInterval)
{
    this->pingInterval = pingInterval ? pingInterval : ENET_PEER_PING_INTERVAL;
}

void ENetPeer::ping()
{
    if (this->state != ENetPeerState::CONNECTED)
    {
        return;
    }

    ENetProtocol command;
    command.header.command = ENET_PROTOCOL_COMMAND_PING | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.header.channelID = 0xFF;

    this->queue_outgoing_command(&command, nullptr, 0, 0);
}

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

int ENetPeer::throttle(enet_uint32 rtt)
{
    if (this->lastRoundTripTime <= this->lastRoundTripTimeVariance)
    {
        this->packetThrottle = this->packetThrottleLimit;
    }
    else if (rtt < this->lastRoundTripTime)
    {
        this->packetThrottle += this->packetThrottleAcceleration;

        if (this->packetThrottle > this->packetThrottleLimit)
        {
            this->packetThrottle = this->packetThrottleLimit;
        }

        return 1;
    }
    else if (rtt > this->lastRoundTripTime + 2 * this->lastRoundTripTimeVariance)
    {
        if (this->packetThrottle > this->packetThrottleDeceleration)
        {
            this->packetThrottle -= this->packetThrottleDeceleration;
        }
        else
        {
            this->packetThrottle = 0;
        }

        return -1;
    }

    return 0;
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
