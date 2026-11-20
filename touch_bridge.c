#define _POSIX_C_SOURCE 200809L
#include "touch_bridge.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>

static pthread_mutex_t bridge_peers_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int bridge_peer_count, bridge_peer_max = 8;
static int *bridge_peers;
static pthread_mutex_t bridge_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t bridge_total_drops[TOUCH_BRIDGE_DROP_COUNT];
static struct timespec bridge_last_log[TOUCH_BRIDGE_DROP_COUNT];
static int bridge_logged[TOUCH_BRIDGE_DROP_COUNT];

int Touch_BridgeSetMaxPeers(unsigned int maximum)
{
    int result = 0;
    pthread_mutex_lock(&bridge_peers_mutex);
    if (!maximum || maximum < bridge_peer_count) result = RTC_ERR_INVALID;
    else bridge_peer_max = maximum;
    pthread_mutex_unlock(&bridge_peers_mutex);
    return result;
}

static char bridge_ice_server[256];

int Touch_BridgeSetIceServer(const char *url)
{
    pthread_mutex_lock(&bridge_peers_mutex);
    if (url && strlen(url) < sizeof(bridge_ice_server)) strcpy(bridge_ice_server, url);
    else bridge_ice_server[0] = 0;
    pthread_mutex_unlock(&bridge_peers_mutex);
    return url && !bridge_ice_server[0] ? RTC_ERR_INVALID : 0;
}

unsigned int Touch_BridgeGetMaxPeers(void)
{
    unsigned int maximum;
    pthread_mutex_lock(&bridge_peers_mutex);
    maximum = bridge_peer_max;
    pthread_mutex_unlock(&bridge_peers_mutex);
    return maximum;
}

int Touch_BridgeCreatePeer(void *context, rtcDescriptionCallbackFunc description,
                           rtcCandidateCallbackFunc candidate, rtcDataChannelCallbackFunc channel)
{
    rtcConfiguration config;
    char ice[sizeof(bridge_ice_server)];
    const char *servers[1] = {ice};
    int peer;
    memset(&config, 0, sizeof(config));
    config.disableAutoNegotiation = true;
    pthread_mutex_lock(&bridge_peers_mutex);
    memcpy(ice, bridge_ice_server, sizeof(ice));
    if (ice[0]) { config.iceServers = servers; config.iceServersCount = 1; }
    if (bridge_peer_count >= bridge_peer_max) {
        pthread_mutex_unlock(&bridge_peers_mutex);
        return RTC_ERR_FAILURE;
    }
    {
        int *peers = (int *)realloc(bridge_peers, (bridge_peer_count + 1) * sizeof(*peers));
        if (!peers) { pthread_mutex_unlock(&bridge_peers_mutex); return RTC_ERR_FAILURE; }
        bridge_peers = peers;
    }
    peer = rtcCreatePeerConnection(&config);
    if (peer < 0) { pthread_mutex_unlock(&bridge_peers_mutex); return peer; }
    bridge_peers[bridge_peer_count++] = peer;
    pthread_mutex_unlock(&bridge_peers_mutex);
    rtcSetUserPointer(peer, context);
    if (rtcSetLocalDescriptionCallback(peer, description) < 0 ||
        rtcSetLocalCandidateCallback(peer, candidate) < 0 ||
        rtcSetDataChannelCallback(peer, channel) < 0) {
        Touch_BridgeDeletePeer(peer);
        return RTC_ERR_FAILURE;
    }
    return peer;
}

int Touch_BridgeDeletePeer(int peer)
{
    unsigned int i;
    int result;
    pthread_mutex_lock(&bridge_peers_mutex);
    for (i = 0; i < bridge_peer_count && bridge_peers[i] != peer; ++i) {}
    if (i == bridge_peer_count) { pthread_mutex_unlock(&bridge_peers_mutex); return RTC_ERR_INVALID; }
    /* Reserve the slot during deletion without holding a lock across callbacks. */
    bridge_peers[i] = -1;
    pthread_mutex_unlock(&bridge_peers_mutex);
    result = rtcDeletePeerConnection(peer);
    pthread_mutex_lock(&bridge_peers_mutex);
    for (i = 0; i < bridge_peer_count && bridge_peers[i] != -1; ++i) {}
    if (result >= 0) bridge_peers[i] = bridge_peers[--bridge_peer_count];
    else bridge_peers[i] = peer;
    if (!bridge_peer_count) { free(bridge_peers); bridge_peers = NULL; }
    pthread_mutex_unlock(&bridge_peers_mutex);
    return result;
}

int Touch_BridgeAcceptChannel(int channel, void *context, rtcOpenCallbackFunc open,
                              rtcMessageCallbackFunc message)
{
    rtcSetUserPointer(channel, context);
    if (rtcSetOpenCallback(channel, open) < 0 || rtcSetMessageCallback(channel, message) < 0)
        return RTC_ERR_FAILURE;
    return 0;
}

int Touch_BridgeOpenChannel(int peer, const char *label, void *context,
                            rtcOpenCallbackFunc open, rtcMessageCallbackFunc message)
{
    rtcDataChannelInit init;
    int channel;
    memset(&init, 0, sizeof(init));
    init.reliability.unordered = true;
    init.reliability.unreliable = true;
    init.reliability.maxRetransmits = 0;
    channel = rtcCreateDataChannelEx(peer, label, &init);
    if (channel >= 0 && Touch_BridgeAcceptChannel(channel, context, open, message) < 0) {
        rtcDeleteDataChannel(channel);
        return RTC_ERR_FAILURE;
    }
    return channel;
}

int Touch_BridgeSend(int channel, const void *bytes, int size)
{
    if (!bytes || size <= 0)
        return RTC_ERR_INVALID;
    return rtcSendMessage(channel, (const char *)bytes, size);
}

int Touch_BridgePack(void *out, int capacity, const struct touch_bridge_address *address,
                     const void *payload, int size)
{
    unsigned char *p = (unsigned char *)out;
    int n, header;
    if (!out || !address || !payload || size <= 0 || size > TOUCH_BRIDGE_MAX_DATAGRAM ||
        !address->port || (address->family != 4 && address->family != 6))
        return RTC_ERR_INVALID;
    n = address->family == 4 ? 4 : 16;
    header = n + 4;
    if (capacity < header || size > capacity - header)
        return RTC_ERR_INVALID;
    p[0] = TOUCH_BRIDGE_VERSION;
    p[1] = address->family;
    memcpy(p + 2, address->address, (size_t)n);
    p[2 + n] = (unsigned char)(address->port >> 8);
    p[3 + n] = (unsigned char)address->port;
    memcpy(p + header, payload, (size_t)size);
    return header + size;
}

int Touch_BridgeUnpack(const void *packet, int size, struct touch_bridge_address *address,
                       const unsigned char **payload, int *payload_size)
{
    const unsigned char *p = (const unsigned char *)packet;
    int n, header;
    if (!p || !address || !payload || !payload_size || size < 2 ||
        p[0] != TOUCH_BRIDGE_VERSION || (p[1] != 4 && p[1] != 6))
        return RTC_ERR_INVALID;
    n = p[1] == 4 ? 4 : 16;
    header = n + 4;
    if (size <= header || size - header > TOUCH_BRIDGE_MAX_DATAGRAM ||
        !(p[2 + n] | p[3 + n]))
        return RTC_ERR_INVALID;
    memset(address, 0, sizeof(*address));
    address->family = p[1];
    memcpy(address->address, p + 2, (size_t)n);
    address->port = (unsigned short)((p[2 + n] << 8) | p[3 + n]);
    *payload = p + header;
    *payload_size = size - header;
    return 0;
}

/* Separate sockets keep IPv4 and IPv6 behavior independent of IPV6_V6ONLY. */
struct touch_bridge_forwarder {
    int ipv4, ipv6;
    pthread_mutex_t mutex;
    struct touch_bridge_address *allowed;
    size_t allowed_count;
    struct touch_bridge_address *masters;
    size_t master_count;
    int allow_loopback;
    unsigned int packet_rate, byte_rate;
    double packet_tokens, byte_tokens;
    struct timespec bucket_time;
    struct timespec activity;
    unsigned int idle_seconds;
    int expired;
    uint64_t drops[TOUCH_BRIDGE_DROP_COUNT];
};

static double bridge_elapsed(struct timespec now, struct timespec then)
{
    return (double)(now.tv_sec - then.tv_sec) + (double)(now.tv_nsec - then.tv_nsec) / 1000000000.0;
}

/* All drop accounting and logging is serialized by the forwarder's mutex. */
static void bridge_drop(struct touch_bridge_forwarder *f, enum touch_bridge_drop_reason reason)
{
    static const char *names[] = {"invalid", "denied", "rate", "io", "idle"};
    struct timespec now;
    ++f->drops[reason];
    pthread_mutex_lock(&bridge_log_mutex);
    ++bridge_total_drops[reason];
    if (!clock_gettime(CLOCK_MONOTONIC, &now) &&
        (!bridge_logged[reason] || bridge_elapsed(now, bridge_last_log[reason]) >= 60)) {
        fprintf(stderr, "bridge: %s drops=%" PRIu64 "\n", names[reason], bridge_total_drops[reason]);
        bridge_logged[reason] = 1;
        bridge_last_log[reason] = now;
    }
    pthread_mutex_unlock(&bridge_log_mutex);
}

/* Caller holds the mutex. Expiry is permanent. */
static int bridge_expired(struct touch_bridge_forwarder *f)
{
    struct timespec now;
    if (!f->expired && !clock_gettime(CLOCK_MONOTONIC, &now) &&
        bridge_elapsed(now, f->activity) >= f->idle_seconds) {
        f->expired = 1;
        bridge_drop(f, TOUCH_BRIDGE_DROP_IDLE);
    }
    return f->expired;
}

static void bridge_activity(struct touch_bridge_forwarder *f)
{
    struct timespec now;
    if (!clock_gettime(CLOCK_MONOTONIC, &now)) f->activity = now;
}

int Touch_BridgeSetIdleTimeout(struct touch_bridge_forwarder *f, unsigned int seconds)
{
    if (!f || !seconds) return RTC_ERR_INVALID;
    pthread_mutex_lock(&f->mutex);
    if (bridge_expired(f)) { pthread_mutex_unlock(&f->mutex); return RTC_ERR_NOT_AVAIL; }
    f->idle_seconds = seconds;
    pthread_mutex_unlock(&f->mutex);
    return 0;
}

uint64_t Touch_BridgeDropCount(struct touch_bridge_forwarder *f, enum touch_bridge_drop_reason reason)
{
    uint64_t count;
    if (!f || reason < 0 || reason >= TOUCH_BRIDGE_DROP_COUNT) return 0;
    pthread_mutex_lock(&f->mutex);
    count = f->drops[reason];
    pthread_mutex_unlock(&f->mutex);
    return count;
}

int Touch_BridgeSetRates(struct touch_bridge_forwarder *f, unsigned int packets, unsigned int bytes)
{
    struct timespec now;
    if (!f || !packets || !bytes || clock_gettime(CLOCK_MONOTONIC, &now)) return RTC_ERR_INVALID;
    pthread_mutex_lock(&f->mutex);
    f->packet_rate = packets; f->byte_rate = bytes;
    f->packet_tokens = packets; f->byte_tokens = bytes; f->bucket_time = now;
    pthread_mutex_unlock(&f->mutex);
    return 0;
}

/* Caller holds the mutex. Failed attempts consume neither bucket. */
static int bridge_take_tokens(struct touch_bridge_forwarder *f, int bytes)
{
    struct timespec now;
    double elapsed;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    elapsed = (double)(now.tv_sec - f->bucket_time.tv_sec) +
              (double)(now.tv_nsec - f->bucket_time.tv_nsec) / 1000000000.0;
    if (elapsed > 0) {
        f->packet_tokens += elapsed * f->packet_rate;
        f->byte_tokens += elapsed * f->byte_rate;
        if (f->packet_tokens > f->packet_rate) f->packet_tokens = f->packet_rate;
        if (f->byte_tokens > f->byte_rate) f->byte_tokens = f->byte_rate;
        f->bucket_time = now;
    }
    if (f->packet_tokens < 1 || f->byte_tokens < bytes) return 0;
    f->packet_tokens -= 1; f->byte_tokens -= bytes;
    return 1;
}

static int bridge_listed(const struct touch_bridge_forwarder *f,
                         const struct touch_bridge_address *address)
{
    size_t i, n = address->family == 4 ? 4 : 16;
    for (i = 0; i < f->allowed_count; ++i)
        if (f->allowed[i].family == address->family &&
            f->allowed[i].port == address->port &&
            !memcmp(f->allowed[i].address, address->address, n))
            return 1;
    return 0;
}

static int bridge_allowed(const struct touch_bridge_forwarder *f,
                          const struct touch_bridge_address *address)
{
    static const unsigned char loopback[4] = {127, 0, 0, 1};
    return (f->allow_loopback && address->family == 4 && !memcmp(address->address, loopback, 4)) ||
           bridge_listed(f, address);
}

int Touch_BridgeSetAllowLoopback(struct touch_bridge_forwarder *f, int enabled)
{
    if (!f) return RTC_ERR_INVALID;
    pthread_mutex_lock(&f->mutex);
    f->allow_loopback = !!enabled;
    pthread_mutex_unlock(&f->mutex);
    return 0;
}

int Touch_BridgeAllowAddress(struct touch_bridge_forwarder *f,
                             const struct touch_bridge_address *address)
{
    struct touch_bridge_address *addresses;
    int result = 0;
    if (!f || !address || !address->port ||
        (address->family != 4 && address->family != 6))
        return RTC_ERR_INVALID;
    pthread_mutex_lock(&f->mutex);
    if (!bridge_listed(f, address)) {
        if (f->allowed_count >= 65536 ||
            !(addresses = (struct touch_bridge_address *)realloc(f->allowed, (f->allowed_count + 1) * sizeof(*addresses))))
            result = RTC_ERR_FAILURE;
        else {
            f->allowed = addresses;
            f->allowed[f->allowed_count++] = *address;
        }
    }
    pthread_mutex_unlock(&f->mutex);
    return result;
}

int Touch_BridgeAllowMaster(struct touch_bridge_forwarder *f,
                            const struct touch_bridge_address *address)
{
    struct touch_bridge_address *masters;
    size_t i, n;
    if (!f || !address || !address->port || (address->family != 4 && address->family != 6))
        return RTC_ERR_INVALID;
    n = address->family == 4 ? 4 : 16;
    pthread_mutex_lock(&f->mutex);
    for (i = 0; i < f->master_count; ++i)
        if (f->masters[i].family == address->family && f->masters[i].port == address->port &&
            !memcmp(f->masters[i].address, address->address, n))
            break;
    if (i == f->master_count) {
        masters = (struct touch_bridge_address *)realloc(f->masters, (i + 1) * sizeof(*masters));
        if (!masters) {
            pthread_mutex_unlock(&f->mutex);
            return RTC_ERR_FAILURE;
        }
        f->masters = masters;
        f->masters[f->master_count++] = *address;
    }
    pthread_mutex_unlock(&f->mutex);
    return Touch_BridgeAllowAddress(f, address);
}

/* Parse only complete DP master response records. An incomplete packet grants
 * nothing, so a truncated or malformed response cannot add partial entries. */
static void bridge_learn_servers(struct touch_bridge_forwarder *f,
                                 const struct touch_bridge_address *source,
                                 const unsigned char *data, size_t length)
{
    static const unsigned char prefix[] = "\xff\xff\xff\xffgetserversResponse";
    static const unsigned char extprefix[] = "\xff\xff\xff\xffgetserversExtResponse";
    size_t i, n, start, pos;
    int extended, master = 0;
    pthread_mutex_lock(&f->mutex);
    n = source->family == 4 ? 4 : 16;
    for (i = 0; i < f->master_count; ++i)
        if (f->masters[i].family == source->family && f->masters[i].port == source->port &&
            !memcmp(f->masters[i].address, source->address, n)) {
            master = 1;
            break;
        }
    pthread_mutex_unlock(&f->mutex);
    if (!master) return;
    if (length >= sizeof(extprefix)-1 && !memcmp(data, extprefix, sizeof(extprefix)-1)) {
        start = sizeof(extprefix)-1;
        extended = 1;
    } else if (length >= sizeof(prefix)-1 && !memcmp(data, prefix, sizeof(prefix)-1)) {
        start = sizeof(prefix)-1;
        extended = 0;
    } else return;
    /* EOT can be either a three-byte marker or the padded seven-byte form. */
    for (pos = start; pos < length;) {
        if ((length - pos == 4 || (length - pos == 7 && !data[pos+4] && !data[pos+5] && !data[pos+6])) && !memcmp(data + pos, "\\EOT", 4)) {
            pos += 4;
            while (pos < length && !data[pos]) ++pos;
            if (pos != length) return;
            break;
        }
        n = data[pos] == '\\' ? 4 : (extended && data[pos] == '/' ? 16 : 0);
        if (!n || length - pos < n + 3 || !(data[pos+n+1] | data[pos+n+2])) return;
        pos += n + 3;
    }
    for (pos = start; pos < length;) {
        struct touch_bridge_address address;
        if ((length - pos == 4 || (length - pos == 7 && !data[pos+4] && !data[pos+5] && !data[pos+6])) && !memcmp(data + pos, "\\EOT", 4)) break;
        n = data[pos] == '\\' ? 4 : 16;
        memset(&address, 0, sizeof(address));
        address.family = n == 4 ? 4 : 6;
        memcpy(address.address, data + pos + 1, n);
        address.port = (unsigned short)((data[pos+n+1] << 8) | data[pos+n+2]);
        Touch_BridgeAllowAddress(f, &address);
        pos += n + 3;
    }
}

static int bridge_socket(int family)
{
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct touch_bridge_forwarder *Touch_BridgeForwarderCreate(void)
{
    struct touch_bridge_forwarder *f = (struct touch_bridge_forwarder *)calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    if (pthread_mutex_init(&f->mutex, NULL)) {
        free(f);
        return NULL;
    }
    f->idle_seconds = 120;
    if (clock_gettime(CLOCK_MONOTONIC, &f->activity)) {
        pthread_mutex_destroy(&f->mutex); free(f); return NULL;
    }
    if (Touch_BridgeSetRates(f, 200, 1048576) < 0) {
        pthread_mutex_destroy(&f->mutex); free(f); return NULL;
    }
    f->ipv4 = bridge_socket(AF_INET);
    f->ipv6 = bridge_socket(AF_INET6);
    if (f->ipv4 < 0 && f->ipv6 < 0) {
        pthread_mutex_destroy(&f->mutex);
        free(f);
        return NULL;
    }
    return f;
}

void Touch_BridgeForwarderDestroy(struct touch_bridge_forwarder *f)
{
    if (!f)
        return;
    if (f->ipv4 >= 0) close(f->ipv4);
    if (f->ipv6 >= 0) close(f->ipv6);
    free(f->allowed);
    free(f->masters);
    pthread_mutex_destroy(&f->mutex);
    free(f);
}

int Touch_BridgeForward(struct touch_bridge_forwarder *f, const void *packet, int size)
{
    struct touch_bridge_address address;
    const unsigned char *payload;
    int len, fd;
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (!f) return RTC_ERR_INVALID;
    pthread_mutex_lock(&f->mutex);
    if (bridge_expired(f)) { pthread_mutex_unlock(&f->mutex); return RTC_ERR_NOT_AVAIL; }
    if (Touch_BridgeUnpack(packet, size, &address, &payload, &len) < 0) {
        bridge_drop(f, TOUCH_BRIDGE_DROP_INVALID);
        pthread_mutex_unlock(&f->mutex); return RTC_ERR_INVALID;
    }
    if (!bridge_allowed(f, &address)) {
        bridge_drop(f, TOUCH_BRIDGE_DROP_DENIED);
        pthread_mutex_unlock(&f->mutex); return RTC_ERR_FAILURE;
    }
    if (!bridge_take_tokens(f, len)) {
        bridge_drop(f, TOUCH_BRIDGE_DROP_RATE);
        pthread_mutex_unlock(&f->mutex);
        return RTC_ERR_FAILURE;
    }
    pthread_mutex_unlock(&f->mutex);
    memset(&storage, 0, sizeof(storage));
    if (address.family == 4) {
        struct sockaddr_in *a = (struct sockaddr_in *)&storage;
        a->sin_family = AF_INET;
        a->sin_port = htons(address.port);
        memcpy(&a->sin_addr, address.address, 4);
        addrlen = sizeof(*a);
        fd = f->ipv4;
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&storage;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(address.port);
        memcpy(&a->sin6_addr, address.address, 16);
        addrlen = sizeof(*a);
        fd = f->ipv6;
    }
    if (fd < 0 || sendto(fd, payload, (size_t)len, 0, (struct sockaddr *)&storage, addrlen) != len) {
        pthread_mutex_lock(&f->mutex); bridge_drop(f, TOUCH_BRIDGE_DROP_IO); pthread_mutex_unlock(&f->mutex);
        return RTC_ERR_FAILURE;
    }
    pthread_mutex_lock(&f->mutex);
    if (!f->expired) bridge_activity(f);
    pthread_mutex_unlock(&f->mutex);
    return 0;
}

static int bridge_poll_socket(struct touch_bridge_forwarder *f, int fd, touch_bridge_reply_fn reply, void *context)
{
    unsigned char payload[TOUCH_BRIDGE_MAX_DATAGRAM + 1];
    unsigned char packet[TOUCH_BRIDGE_MAX_DATAGRAM + 20];
    struct sockaddr_storage storage;
    struct touch_bridge_address address;
    int count = 0;
    if (fd < 0)
        return 0;
    for (;;) {
        socklen_t addrlen = sizeof(storage);
        ssize_t len = recvfrom(fd, payload, sizeof(payload), 0, (struct sockaddr *)&storage, &addrlen);
        int size;
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return count;
            if (errno == EINTR) continue;
            return RTC_ERR_FAILURE;
        }
        if (len == 0 || len > TOUCH_BRIDGE_MAX_DATAGRAM) {
            pthread_mutex_lock(&f->mutex); bridge_drop(f, TOUCH_BRIDGE_DROP_INVALID); pthread_mutex_unlock(&f->mutex);
            continue;
        }
        memset(&address, 0, sizeof(address));
        if (storage.ss_family == AF_INET) {
            const struct sockaddr_in *a = (const struct sockaddr_in *)&storage;
            address.family = 4;
            address.port = ntohs(a->sin_port);
            memcpy(address.address, &a->sin_addr, 4);
        } else if (storage.ss_family == AF_INET6) {
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&storage;
            address.family = 6;
            address.port = ntohs(a->sin6_port);
            memcpy(address.address, &a->sin6_addr, 16);
        } else {
            pthread_mutex_lock(&f->mutex); bridge_drop(f, TOUCH_BRIDGE_DROP_INVALID); pthread_mutex_unlock(&f->mutex);
            continue;
        }
        pthread_mutex_lock(&f->mutex);
        if (bridge_expired(f)) { pthread_mutex_unlock(&f->mutex); return RTC_ERR_NOT_AVAIL; }
        if (!bridge_allowed(f, &address)) { bridge_drop(f, TOUCH_BRIDGE_DROP_DENIED); size = 0; }
        else if (!bridge_take_tokens(f, (int)len)) { bridge_drop(f, TOUCH_BRIDGE_DROP_RATE); size = 0; }
        else { bridge_activity(f); size = 1; }
        pthread_mutex_unlock(&f->mutex);
        if (!size) continue;
        bridge_learn_servers(f, &address, payload, (size_t)len);
        size = Touch_BridgePack(packet, sizeof(packet), &address, payload, (int)len);
        if (size > 0) {
            reply(context, packet, size);
            ++count;
        }
    }
}

int Touch_BridgePoll(struct touch_bridge_forwarder *f, touch_bridge_reply_fn reply, void *context)
{
    int a, b;
    if (!f || !reply) return RTC_ERR_INVALID;
    pthread_mutex_lock(&f->mutex);
    a = bridge_expired(f);
    pthread_mutex_unlock(&f->mutex);
    if (a) return RTC_ERR_NOT_AVAIL;
    a = bridge_poll_socket(f, f->ipv4, reply, context);
    if (a == RTC_ERR_NOT_AVAIL) return a;
    b = bridge_poll_socket(f, f->ipv6, reply, context);
    if (b == RTC_ERR_NOT_AVAIL) return b;
    return a < 0 || b < 0 ? RTC_ERR_FAILURE : a + b;
}

void Touch_BridgeForwardMessage(int channel, const char *message, int size, void *context)
{
    (void)channel;
    /* Negative sizes represent text messages and are rejected by Unpack. */
    Touch_BridgeForward((struct touch_bridge_forwarder *)context, message, size);
}

static void bridge_send_reply(void *context, const void *packet, int size)
{
    int channel = *(const int *)context;
    Touch_BridgeSend(channel, packet, size);
}

int Touch_BridgePollChannel(struct touch_bridge_forwarder *f, int channel)
{
    return Touch_BridgePoll(f, bridge_send_reply, &channel);
}

int Touch_BridgePollPeer(struct touch_bridge_forwarder *f, int peer, int channel)
{
    int result = Touch_BridgePollChannel(f, channel);
    if (result == RTC_ERR_NOT_AVAIL && Touch_BridgeDeletePeer(peer) < 0)
        return RTC_ERR_FAILURE;
    return result;
}
