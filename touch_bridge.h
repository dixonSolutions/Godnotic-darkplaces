#ifndef TOUCH_BRIDGE_H
#define TOUCH_BRIDGE_H

#include <rtc/rtc.h>
#include <stddef.h>
#include <stdint.h>

/* Callbacks execute on libdatachannel threads; the caller owns their context. */
int Touch_BridgeCreatePeer(void *context, rtcDescriptionCallbackFunc description,
                           rtcCandidateCallbackFunc candidate, rtcDataChannelCallbackFunc channel);
/* Every successfully created peer must be released through this function. */
int Touch_BridgeDeletePeer(int peer);
/* One STUN server for peers created afterwards ("stun:host:port"); NULL clears. */
int Touch_BridgeSetIceServer(const char *url);
unsigned int Touch_BridgeGetMaxPeers(void);
/* A limit change cannot be smaller than the current number of peers. */
int Touch_BridgeSetMaxPeers(unsigned int maximum);
int Touch_BridgeOpenChannel(int peer, const char *label, void *context,
                            rtcOpenCallbackFunc open, rtcMessageCallbackFunc message);
int Touch_BridgeAcceptChannel(int channel, void *context, rtcOpenCallbackFunc open,
                              rtcMessageCallbackFunc message);
int Touch_BridgeSend(int channel, const void *bytes, int size);


/* Wire format: version 1, address family (4 or 6), address bytes, big-endian
 * port, and a nonempty UDP payload. The payload is never modified. */
#define TOUCH_BRIDGE_VERSION 1
#define TOUCH_BRIDGE_MAX_DATAGRAM 65507
struct touch_bridge_address {
    unsigned char family;
    unsigned char address[16];
    unsigned short port;
};
int Touch_BridgePack(void *out, int capacity, const struct touch_bridge_address *address,
                     const void *payload, int size);
int Touch_BridgeUnpack(const void *packet, int size, struct touch_bridge_address *address,
                       const unsigned char **payload, int *payload_size);

/* A forwarder belongs to one peer. Call Poll regularly from its owning thread;
 * the callback receives a complete framed reply, valid only during the call. */
struct touch_bridge_forwarder;
enum touch_bridge_drop_reason {
    TOUCH_BRIDGE_DROP_INVALID, TOUCH_BRIDGE_DROP_DENIED, TOUCH_BRIDGE_DROP_RATE,
    TOUCH_BRIDGE_DROP_IO, TOUCH_BRIDGE_DROP_IDLE, TOUCH_BRIDGE_DROP_COUNT
};
/* Defaults to 120 seconds. Accepted traffic in either direction resets the
 * timer; rejected traffic cannot keep a peer alive. Zero is invalid. */
int Touch_BridgeSetIdleTimeout(struct touch_bridge_forwarder *forwarder, unsigned int seconds);
uint64_t Touch_BridgeDropCount(struct touch_bridge_forwarder *forwarder,
                               enum touch_bridge_drop_reason reason);
typedef void (*touch_bridge_reply_fn)(void *context, const void *packet, int size);
struct touch_bridge_forwarder *Touch_BridgeForwarderCreate(void);
/* Test-only exception for exactly IPv4 127.0.0.1, on any port. */
int Touch_BridgeSetAllowLoopback(struct touch_bridge_forwarder *forwarder, int enabled);
/* Shared inbound/outbound token buckets, initially full. Rates are also burst
 * capacities, in packets and UDP payload bytes per second. Defaults: 200 and
 * 1048576. Both rates must be positive; setting them resets the buckets. */
int Touch_BridgeSetRates(struct touch_bridge_forwarder *forwarder,
                         unsigned int packets_per_second, unsigned int bytes_per_second);
/* Engine peers must use this constructor after master cvars are loaded. */
void Touch_BridgeInit(void);
void Touch_BridgeSignalingInit(void);
void Touch_BridgeSignalingFrame(void);
void Touch_BridgeSignalingShutdown(void);
/* The opt-in web relay (bridge_relay cvar), driven by the signaling frame. */
void Touch_BridgeRelayInit(void);
void Touch_BridgeRelayFrame(void);
void Touch_BridgeRelayShutdown(void);
struct touch_bridge_forwarder *Touch_BridgeCreateConfiguredForwarder(void);
/* sv_master1..4 and sv_masterextra1..3, resolved; 0 when unset or unresolvable. */
#define TOUCH_BRIDGE_MASTER_CVARS 7
int Touch_BridgeConfiguredMaster(size_t index, struct touch_bridge_address *address);
void Touch_BridgeForwarderDestroy(struct touch_bridge_forwarder *forwarder);
/* Destinations are denied by default. This primitive does not establish that
 * an address is a master or an advertised server; the caller must do that. */
int Touch_BridgeAllowAddress(struct touch_bridge_forwarder *forwarder,
                             const struct touch_bridge_address *address);
/* Register a configured master. Server addresses are learned only from its replies. */
int Touch_BridgeAllowMaster(struct touch_bridge_forwarder *forwarder,
                            const struct touch_bridge_address *address);
int Touch_BridgeForward(struct touch_bridge_forwarder *forwarder, const void *packet, int size);
int Touch_BridgePoll(struct touch_bridge_forwarder *forwarder, touch_bridge_reply_fn reply,
                     void *context);
/* Install as the channel's message callback with the forwarder as context.
 * Keep the forwarder alive until callbacks have stopped. */
void Touch_BridgeForwardMessage(int channel, const char *message, int size, void *context);
int Touch_BridgePollChannel(struct touch_bridge_forwarder *forwarder, int channel);
/* Poll from the owner thread regularly, even without traffic. On idle expiry,
 * deletes peer and returns RTC_ERR_NOT_AVAIL; the caller must then stop channel
 * callbacks and destroy the forwarder. Other errors do not delete the peer. */
int Touch_BridgePollPeer(struct touch_bridge_forwarder *forwarder, int peer, int channel);

#endif
