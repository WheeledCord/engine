/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "network.h"

#include "enet/enet.h"
#include <stdlib.h>
#include <string.h>

CoreNetWriter CoreNetWriterBegin(void *data, size_t capacity)
{
    CoreNetWriter writer = {(unsigned char *)data, capacity, 0, false};
    if (!data && capacity)
        writer.failed = true;
    return writer;
}

static bool Write(CoreNetWriter *writer, const unsigned char *bytes, size_t size)
{
    if (!writer || writer->failed || !bytes || size > writer->capacity - writer->size)
    {
        if (writer)
            writer->failed = true;
        return false;
    }
    memcpy(writer->data + writer->size, bytes, size);
    writer->size += size;
    return true;
}

bool CoreNetWriteU8(CoreNetWriter *writer, uint8_t value)
{
    return Write(writer, &value, 1);
}

bool CoreNetWriteU16(CoreNetWriter *writer, uint16_t value)
{
    unsigned char bytes[2] = {(unsigned char)(value >> 8), (unsigned char)value};
    return Write(writer, bytes, sizeof bytes);
}

bool CoreNetWriteU32(CoreNetWriter *writer, uint32_t value)
{
    unsigned char bytes[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16),
                              (unsigned char)(value >> 8), (unsigned char)value};
    return Write(writer, bytes, sizeof bytes);
}

bool CoreNetWriteF32(CoreNetWriter *writer, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return CoreNetWriteU32(writer, bits);
}

CoreNetReader CoreNetReaderBegin(const void *data, size_t size)
{
    CoreNetReader reader = {(const unsigned char *)data, size, 0, false};
    if (!data && size)
        reader.failed = true;
    return reader;
}

static bool Read(CoreNetReader *reader, unsigned char *bytes, size_t size)
{
    if (!reader || reader->failed || !bytes || size > reader->size - reader->at)
    {
        if (reader)
            reader->failed = true;
        return false;
    }
    memcpy(bytes, reader->data + reader->at, size);
    reader->at += size;
    return true;
}

bool CoreNetReadU8(CoreNetReader *reader, uint8_t *value)
{
    uint8_t read;
    if (!Read(reader, &read, 1))
        return false;
    *value = read;
    return true;
}

bool CoreNetReadU16(CoreNetReader *reader, uint16_t *value)
{
    unsigned char bytes[2];
    if (!Read(reader, bytes, sizeof bytes))
        return false;
    *value = (uint16_t)((uint16_t)bytes[0] << 8 | (uint16_t)bytes[1]);
    return true;
}

bool CoreNetReadU32(CoreNetReader *reader, uint32_t *value)
{
    unsigned char bytes[4];
    if (!Read(reader, bytes, sizeof bytes))
        return false;
    *value = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 |
             (uint32_t)bytes[3];
    return true;
}

bool CoreNetReadF32(CoreNetReader *reader, float *value)
{
    uint32_t bits;
    if (!CoreNetReadU32(reader, &bits))
        return false;
    memcpy(value, &bits, sizeof bits);
    return true;
}

static bool ValidConfig(const CoreNetEndpoint *endpoint, int channelCount)
{
    return endpoint && !endpoint->host && !endpoint->initialized && channelCount > 0 &&
           channelCount <= ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
}

static bool BeginEndpoint(CoreNetEndpoint *endpoint)
{
    if (enet_initialize() != 0)
        return false;
    endpoint->initialized = true;
    return true;
}

static void FailedEndpoint(CoreNetEndpoint *endpoint)
{
    if (endpoint->initialized)
        enet_deinitialize();
    memset(endpoint, 0, sizeof *endpoint);
}

bool CoreNetOpenServer(CoreNetEndpoint *endpoint, uint16_t port, int maxPeers, int channelCount)
{
    if (!ValidConfig(endpoint, channelCount) || maxPeers <= 0 ||
        maxPeers > ENET_PROTOCOL_MAXIMUM_PEER_ID)
        return false;
    if (!BeginEndpoint(endpoint))
        return false;
    ENetAddress address = {ENET_HOST_ANY, port};
    endpoint->host = enet_host_create(&address, (size_t)maxPeers, (size_t)channelCount, 0, 0);
    if (!endpoint->host)
    {
        FailedEndpoint(endpoint);
        return false;
    }
    return true;
}

bool CoreNetOpenClient(CoreNetEndpoint *endpoint, int channelCount)
{
    if (!ValidConfig(endpoint, channelCount))
        return false;
    if (!BeginEndpoint(endpoint))
        return false;
    endpoint->host = enet_host_create(NULL, 1, (size_t)channelCount, 0, 0);
    if (!endpoint->host)
    {
        FailedEndpoint(endpoint);
        return false;
    }
    return true;
}

static ENetHost *Host(const CoreNetEndpoint *endpoint)
{
    return endpoint ? (ENetHost *)endpoint->host : NULL;
}

static ENetPeer *Peer(const CoreNetEndpoint *endpoint, CoreNetPeer peer)
{
    ENetHost *host = Host(endpoint);
    if (!host || peer >= host->peerCount)
        return NULL;
    return &host->peers[peer];
}

static CoreNetPeer PeerId(const ENetHost *host, const ENetPeer *peer)
{
    if (!host || !peer || peer < host->peers || peer >= host->peers + host->peerCount)
        return CORE_NET_PEER_NONE;
    return (CoreNetPeer)(peer - host->peers);
}

CoreNetPeer CoreNetConnect(CoreNetEndpoint *endpoint, const char *hostname, uint16_t port)
{
    ENetHost *host = Host(endpoint);
    if (!host || !hostname || !hostname[0] || !port)
        return CORE_NET_PEER_NONE;
    ENetAddress address = {0};
    address.port = port;
    if (enet_address_set_host(&address, hostname) != 0)
        return CORE_NET_PEER_NONE;
    ENetPeer *peer = enet_host_connect(host, &address, host->channelLimit, 0);
    return PeerId(host, peer);
}

void CoreNetClose(CoreNetEndpoint *endpoint)
{
    if (!endpoint)
        return;
    if (endpoint->host)
        enet_host_destroy((ENetHost *)endpoint->host);
    if (endpoint->initialized)
        enet_deinitialize();
    memset(endpoint, 0, sizeof *endpoint);
}

bool CoreNetPoll(CoreNetEndpoint *endpoint, uint32_t timeoutMs, CoreNetEvent *event)
{
    ENetHost *host = Host(endpoint);
    if (!host || !event)
        return false;
    *event = (CoreNetEvent){0};
    event->peer = CORE_NET_PEER_NONE;
    ENetEvent incoming = {0};
    int result = enet_host_service(host, &incoming, timeoutMs);
    if (result == 0)
        return false;
    if (result < 0)
    {
        event->type = CORE_NET_EVENT_ERROR;
        return true;
    }
    event->peer = PeerId(host, incoming.peer);
    event->channel = incoming.channelID;
    if (incoming.type == ENET_EVENT_TYPE_CONNECT)
        event->type = CORE_NET_EVENT_CONNECTED;
    else if (incoming.type == ENET_EVENT_TYPE_DISCONNECT)
        event->type = CORE_NET_EVENT_DISCONNECTED;
    else if (incoming.type == ENET_EVENT_TYPE_RECEIVE)
    {
        event->type = CORE_NET_EVENT_RECEIVED;
        event->size = incoming.packet->dataLength;
        if (event->size > CORE_NET_MAX_MESSAGE_SIZE)
            event->type = CORE_NET_EVENT_ERROR;
        else if (event->size)
        {
            event->data = malloc(event->size);
            if (event->data)
                memcpy(event->data, incoming.packet->data, event->size);
            else
            {
                event->size = 0;
                event->type = CORE_NET_EVENT_ERROR;
            }
        }
        enet_packet_destroy(incoming.packet);
    }
    else
        event->type = CORE_NET_EVENT_ERROR;
    return true;
}

void CoreNetEventFree(CoreNetEvent *event)
{
    if (!event)
        return;
    free(event->data);
    *event = (CoreNetEvent){0};
    event->peer = CORE_NET_PEER_NONE;
}

static bool ValidMessage(const ENetHost *host, uint8_t channel, const void *data, size_t size)
{
    return host && channel < host->channelLimit && (!size || data) &&
           size <= CORE_NET_MAX_MESSAGE_SIZE;
}

bool CoreNetSend(CoreNetEndpoint *endpoint, CoreNetPeer peerId, uint8_t channel, const void *data,
                 size_t size, bool reliable)
{
    ENetHost *host = Host(endpoint);
    ENetPeer *peer = Peer(endpoint, peerId);
    if (!ValidMessage(host, channel, data, size) || !peer ||
        peer->state != ENET_PEER_STATE_CONNECTED)
        return false;
    ENetPacket *packet =
        enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet)
        return false;
    if (enet_peer_send(peer, channel, packet) != 0)
    {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

bool CoreNetBroadcast(CoreNetEndpoint *endpoint, uint8_t channel, const void *data, size_t size,
                      bool reliable)
{
    ENetHost *host = Host(endpoint);
    if (!ValidMessage(host, channel, data, size))
        return false;
    ENetPacket *packet =
        enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet)
        return false;
    enet_host_broadcast(host, channel, packet);
    return true;
}

void CoreNetFlush(CoreNetEndpoint *endpoint)
{
    ENetHost *host = Host(endpoint);
    if (host)
        enet_host_flush(host);
}

uint16_t CoreNetPort(const CoreNetEndpoint *endpoint)
{
    ENetHost *host = Host(endpoint);
    return host ? host->address.port : 0;
}

uint32_t CoreNetRoundTripTime(const CoreNetEndpoint *endpoint, CoreNetPeer peerId)
{
    ENetPeer *peer = Peer(endpoint, peerId);
    return peer && peer->state == ENET_PEER_STATE_CONNECTED ? peer->roundTripTime : 0;
}
