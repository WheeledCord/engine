/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_NETWORK_H
#define CORE_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CORE_NET_PEER_NONE UINT32_MAX
#define CORE_NET_MAX_MESSAGE_SIZE (1024u * 1024u)

typedef uint32_t CoreNetPeer;

typedef struct CoreNetEndpoint
{
    void *host;
    bool initialized;
} CoreNetEndpoint;

typedef enum CoreNetEventType
{
    CORE_NET_EVENT_NONE,
    CORE_NET_EVENT_CONNECTED,
    CORE_NET_EVENT_DISCONNECTED,
    CORE_NET_EVENT_RECEIVED,
    CORE_NET_EVENT_ERROR
} CoreNetEventType;

typedef struct CoreNetEvent
{
    CoreNetEventType type;
    CoreNetPeer peer;
    uint8_t channel;
    unsigned char *data;
    size_t size;
} CoreNetEvent;

typedef struct CoreNetWriter
{
    unsigned char *data;
    size_t capacity, size;
    bool failed;
} CoreNetWriter;

typedef struct CoreNetReader
{
    const unsigned char *data;
    size_t size, at;
    bool failed;
} CoreNetReader;

/** @brief Begins writing an explicitly encoded network message.
 * @param data Caller-owned destination bytes.
 * @param capacity Writable byte count.
 * @return Writer positioned at byte zero; writes fail safely after capacity is exhausted. */
CoreNetWriter CoreNetWriterBegin(void *data, size_t capacity);

/** @brief Appends one unsigned byte to a network message.
 * @param writer Writer to advance.
 * @param value Value to append.
 * @return True on success; false marks writer failed and writes nothing. */
bool CoreNetWriteU8(CoreNetWriter *writer, uint8_t value);

/** @brief Appends an unsigned 16-bit integer in network byte order.
 * @param writer Writer to advance.
 * @param value Value to append.
 * @return True on success; false marks writer failed and writes nothing. */
bool CoreNetWriteU16(CoreNetWriter *writer, uint16_t value);

/** @brief Appends an unsigned 32-bit integer in network byte order.
 * @param writer Writer to advance.
 * @param value Value to append.
 * @return True on success; false marks writer failed and writes nothing. */
bool CoreNetWriteU32(CoreNetWriter *writer, uint32_t value);

/** @brief Appends an IEEE-754 32-bit float in network byte order.
 * @param writer Writer to advance.
 * @param value Finite or non-finite bit pattern to append unchanged.
 * @return True on success; false marks writer failed and writes nothing. */
bool CoreNetWriteF32(CoreNetWriter *writer, float value);

/** @brief Begins reading an explicitly encoded network message.
 * @param data Borrowed source bytes.
 * @param size Readable byte count.
 * @return Reader positioned at byte zero; reads fail safely at the end. */
CoreNetReader CoreNetReaderBegin(const void *data, size_t size);

/** @brief Reads one unsigned byte from a network message.
 * @param reader Reader to advance.
 * @param value Destination value.
 * @return True on success; false marks reader failed and leaves value unchanged. */
bool CoreNetReadU8(CoreNetReader *reader, uint8_t *value);

/** @brief Reads one network-byte-order unsigned 16-bit integer.
 * @param reader Reader to advance.
 * @param value Destination value.
 * @return True on success; false marks reader failed and leaves value unchanged. */
bool CoreNetReadU16(CoreNetReader *reader, uint16_t *value);

/** @brief Reads one network-byte-order unsigned 32-bit integer.
 * @param reader Reader to advance.
 * @param value Destination value.
 * @return True on success; false marks reader failed and leaves value unchanged. */
bool CoreNetReadU32(CoreNetReader *reader, uint32_t *value);

/** @brief Reads one network-byte-order IEEE-754 32-bit float.
 * @param reader Reader to advance.
 * @param value Destination value.
 * @return True on success; false marks reader failed and leaves value unchanged. */
bool CoreNetReadF32(CoreNetReader *reader, float *value);

/** @brief Opens a nonblocking server endpoint over ENet's reliable-UDP transport.
 * @param endpoint Zeroed caller-owned endpoint storage.
 * @param port UDP port in host byte order; zero asks the operating system for an available port.
 * @param maxPeers Maximum simultaneous peers; must be positive.
 * @param channelCount Number of independently sequenced channels; must be 1 through 255.
 * @return True on success; false leaves endpoint safe to close. */
bool CoreNetOpenServer(CoreNetEndpoint *endpoint, uint16_t port, int maxPeers, int channelCount);

/** @brief Opens a nonblocking client endpoint over ENet's reliable-UDP transport.
 * @param endpoint Zeroed caller-owned endpoint storage.
 * @param channelCount Number of independently sequenced channels; must be 1 through 255.
 * @return True on success; false leaves endpoint safe to close. */
bool CoreNetOpenClient(CoreNetEndpoint *endpoint, int channelCount);

/** @brief Begins connecting a client endpoint to a server.
 * @param endpoint Open client endpoint.
 * @param hostname DNS name or numeric address to resolve synchronously.
 * @param port Server UDP port in host byte order; zero is rejected.
 * @return A local peer handle, or CORE_NET_PEER_NONE on invalid input or resolution failure. */
CoreNetPeer CoreNetConnect(CoreNetEndpoint *endpoint, const char *hostname, uint16_t port);

/** @brief Releases an endpoint and all of its peers and queued packets.
 * @param endpoint Endpoint to close; NULL and already-closed endpoints are accepted.
 * @return No value. */
void CoreNetClose(CoreNetEndpoint *endpoint);

/** @brief Polls one connection or packet event.
 * @param endpoint Open endpoint.
 * @param timeoutMs Maximum milliseconds to wait; zero performs a nonblocking poll.
 * @param event Receives an event. Received packet data is owned by event and must be released.
 * @return True when an event was produced, including CORE_NET_EVENT_ERROR; false on timeout.
 *
 * Call CoreNetEventFree after every produced event. A peer handle is local to its endpoint and
 * remains stable only until that peer disconnects or the endpoint closes. */
bool CoreNetPoll(CoreNetEndpoint *endpoint, uint32_t timeoutMs, CoreNetEvent *event);

/** @brief Releases packet storage owned by a polled event and clears it.
 * @param event Event to release; NULL is accepted.
 * @return No value. */
void CoreNetEventFree(CoreNetEvent *event);

/** @brief Queues one message for a connected peer.
 * @param endpoint Open endpoint that owns peer.
 * @param peer Local peer handle from connect or poll.
 * @param channel Sequencing channel selected when the endpoint was opened.
 * @param data Message bytes; may be NULL only when size is zero.
 * @param size Byte count, at most CORE_NET_MAX_MESSAGE_SIZE.
 * @param reliable True requests acknowledged, retransmitted delivery; false sends a sequenced
 * unreliable message suitable for replaceable snapshots.
 * @return True when queued; false on invalid input, allocation failure, or disconnected peer. */
bool CoreNetSend(CoreNetEndpoint *endpoint, CoreNetPeer peer, uint8_t channel, const void *data,
                 size_t size, bool reliable);

/** @brief Queues one message for every connected peer.
 * @param endpoint Open endpoint.
 * @param channel Sequencing channel selected when the endpoint was opened.
 * @param data Message bytes; may be NULL only when size is zero.
 * @param size Byte count, at most CORE_NET_MAX_MESSAGE_SIZE.
 * @param reliable True requests acknowledged, retransmitted delivery; false sends a sequenced
 * unreliable message.
 * @return True when a packet was queued; false on invalid input or allocation failure. */
bool CoreNetBroadcast(CoreNetEndpoint *endpoint, uint8_t channel, const void *data, size_t size,
                      bool reliable);

/** @brief Immediately submits queued packets to the operating system.
 * @param endpoint Open endpoint; NULL is accepted.
 * @return No value. Normal applications may rely on CoreNetPoll to flush automatically. */
void CoreNetFlush(CoreNetEndpoint *endpoint);

/** @brief Returns the UDP port actually bound by an endpoint.
 * @param endpoint Open endpoint.
 * @return Bound port in host byte order, or zero for an invalid endpoint. */
uint16_t CoreNetPort(const CoreNetEndpoint *endpoint);

/** @brief Returns the smoothed round-trip estimate for a connected peer.
 * @param endpoint Open endpoint that owns peer.
 * @param peer Local peer handle.
 * @return Round-trip milliseconds, or zero for an invalid or disconnected peer. */
uint32_t CoreNetRoundTripTime(const CoreNetEndpoint *endpoint, CoreNetPeer peer);

#endif
