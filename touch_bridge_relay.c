/* The opt-in web relay: public MQTT signaling + touch_bridge forwarding.
 * See touch_bridge_relay.h for the message formats. */
#define _POSIX_C_SOURCE 200809L
#include "touch_bridge_relay.h"
#include "touch_bridge_mqtt.h"
#include "touch_bridge.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RELAY_MAX_BROKERS 4
#define RELAY_MAX_SDP (64 * 1024)
#define RELAY_DEFAULT_PREFIX "godnotic/relay/v1"

struct relay_broker {
    char *url;
    struct touch_mqtt *mqtt;
    double retry_at, backoff;
    int ready, announced;
};

struct relay_session {
    struct relay_session *next;
    struct touch_relay *relay;
    struct touch_bridge_forwarder *forwarder;
    int peer, broker;
    atomic_int channel, gathered, failed, opened;
    int answered;
    double created;
    char client[65];
};

struct touch_relay {
    struct touch_relay_config config;
    char *brokers_text, *prefix, *masters, *stun;
    char id[17];
    struct relay_broker brokers[RELAY_MAX_BROKERS];
    int broker_count;
    struct relay_session *sessions;
    int peers;
    double tokens, tokens_at;
};

static double relay_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void relay_log(struct touch_relay *r, const char *fmt, const char *a, const char *b)
{
    char text[512];
    if (!r->config.log) return;
    snprintf(text, sizeof(text), fmt, a ? a : "", b ? b : "");
    r->config.log(r->config.context, text);
}

static int valid_client(const char *s, int n)
{
    int i;
    if (n < 1 || n > 64) return 0;
    for (i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

int Touch_RelayParseOffer(const char *text, int size, char *client, const char **sdp)
{
    static const char magic[] = "GODNOTIC-OFFER 1\n";
    const char *p = text, *end = text + size, *body = NULL;
    int have_from = 0;
    if (size <= (int)sizeof(magic) || size > RELAY_MAX_SDP || memchr(text, 0, (size_t)size) ||
        memcmp(text, magic, sizeof(magic) - 1))
        return -1;
    p += sizeof(magic) - 1;
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!eol) return -1;
        if (eol == p) { body = eol + 1; break; }
        if (eol - p > 5 && !memcmp(p, "from ", 5)) {
            if (!valid_client(p + 5, (int)(eol - p - 5))) return -1;
            memcpy(client, p + 5, (size_t)(eol - p - 5));
            client[eol - p - 5] = 0;
            have_from = 1;
        }
        p = eol + 1;
    }
    if (!have_from || !body || end - body < 10 || memcmp(body, "v=0", 3)) return -1;
    *sdp = body;
    return 0;
}

static void topic(struct touch_relay *r, char *out, size_t size, const char *kind, const char *who)
{
    snprintf(out, size, "%s/%s/%s", r->prefix, kind, who);
}

static void publish_presence(struct touch_relay *r, struct relay_broker *b)
{
    char t[256], text[128];
    int n = snprintf(text, sizeof(text), "GODNOTIC-RELAY 1\npeers %d\nmax %u\n\n", r->peers, Touch_BridgeGetMaxPeers());
    topic(r, t, sizeof(t), "online", r->id);
    if (Touch_MqttPublish(b->mqtt, t, text, n, 1) == 0) b->announced = r->peers;
}

static void publish_answer(struct touch_relay *r, int broker, const char *client, const char *extra,
                           const char *sdp)
{
    char t[256];
    size_t cap = 512 + strlen(r->masters) * 2 + (extra ? strlen(extra) : 0) + (sdp ? strlen(sdp) : 0);
    char *text = (char *)malloc(cap), *at;
    const char *m = r->masters;
    struct relay_broker *b = &r->brokers[broker];
    if (!text) return;
    at = text + snprintf(text, cap, "GODNOTIC-ANSWER 1\nrelay %s\n", r->id);
    if (extra) at += snprintf(at, cap - (size_t)(at - text), "%s\n", extra);
    else while (*m) {
        size_t n;
        while (*m == ' ') ++m;
        n = strcspn(m, " ");
        if (n && n < 64) at += snprintf(at, cap - (size_t)(at - text), "master %.*s\n", (int)n, m);
        m += n;
    }
    at += snprintf(at, cap - (size_t)(at - text), "\n%s", sdp ? sdp : "");
    topic(r, t, sizeof(t), "answer", client);
    if (b->mqtt && b->ready) Touch_MqttPublish(b->mqtt, t, text, (int)(at - text), 0);
    free(text);
}

/* libdatachannel callbacks: only atomics. */
static void s_description(int pc, const char *sdp, const char *type, void *p) { (void)pc; (void)sdp; (void)type; (void)p; }
static void s_candidate(int pc, const char *sdp, const char *mid, void *p) { (void)pc; (void)sdp; (void)mid; (void)p; }
static void s_opened(int channel, void *p) { (void)channel; atomic_store(&((struct relay_session *)p)->opened, 1); }
static void s_message(int channel, const char *data, int size, void *p)
{
    Touch_BridgeForwardMessage(channel, data, size, ((struct relay_session *)p)->forwarder);
}
static void s_channel(int pc, int channel, void *p)
{
    struct relay_session *s = (struct relay_session *)p;
    int expected = -1;
    (void)pc;
    if (!atomic_compare_exchange_strong(&s->channel, &expected, channel)) {
        rtcDeleteDataChannel(channel);
        return;
    }
    if (Touch_BridgeAcceptChannel(channel, s, s_opened, s_message) < 0) atomic_store(&s->failed, 1);
    else if (rtcIsOpen(channel)) atomic_store(&s->opened, 1); /* an offered channel arrives open */
}
static void s_gathering(int pc, rtcGatheringState state, void *p)
{
    (void)pc;
    if (state == RTC_GATHERING_COMPLETE) atomic_store(&((struct relay_session *)p)->gathered, 1);
}
static void s_state(int pc, rtcState state, void *p)
{
    (void)pc;
    if (state == RTC_FAILED || state == RTC_CLOSED) atomic_store(&((struct relay_session *)p)->failed, 1);
}

static void session_free(struct relay_session *s, int peer_deleted)
{
    int channel;
    if (s->peer >= 0 && !peer_deleted) Touch_BridgeDeletePeer(s->peer);
    channel = atomic_load(&s->channel);
    if (channel >= 0) rtcDeleteDataChannel(channel);
    if (s->forwarder) Touch_BridgeForwarderDestroy(s->forwarder);
    free(s);
}

static int take_offer_token(struct touch_relay *r)
{
    double now = relay_now(), rate = r->config.offers_per_minute / 60.0;
    r->tokens += (now - r->tokens_at) * rate;
    if (r->tokens > r->config.offers_per_minute) r->tokens = r->config.offers_per_minute;
    r->tokens_at = now;
    if (r->tokens < 1) return 0;
    r->tokens -= 1;
    return 1;
}

static void handle_offer(struct touch_relay *r, int broker, const unsigned char *payload, int size)
{
    char client[65];
    const char *sdp;
    char *copy;
    struct relay_session *s;
    if (size <= 0 || size > RELAY_MAX_SDP) return;
    copy = (char *)malloc((size_t)size + 1);
    if (!copy) return;
    memcpy(copy, payload, (size_t)size);
    copy[size] = 0;
    if (Touch_RelayParseOffer(copy, size, client, &sdp) < 0) { free(copy); return; }
    for (s = r->sessions; s; s = s->next)
        if (!strcmp(s->client, client)) { free(copy); return; } /* a repeat: one session each */
    if (!take_offer_token(r)) {
        publish_answer(r, broker, client, "error busy", NULL);
        free(copy);
        return;
    }
    s = (struct relay_session *)calloc(1, sizeof(*s));
    if (!s) { free(copy); return; }
    s->relay = r;
    s->broker = broker;
    s->peer = -1;
    s->created = relay_now();
    memcpy(s->client, client, sizeof(client));
    atomic_init(&s->channel, -1); atomic_init(&s->gathered, 0);
    atomic_init(&s->failed, 0); atomic_init(&s->opened, 0);
    s->forwarder = r->config.make_forwarder ? r->config.make_forwarder(r->config.context) : NULL;
    if (s->forwarder) s->peer = Touch_BridgeCreatePeer(s, s_description, s_candidate, s_channel);
    if (s->peer < 0) {
        publish_answer(r, broker, client, s->forwarder ? "error full" : "error internal", NULL);
        session_free(s, 1);
        free(copy);
        return;
    }
    if (rtcSetGatheringStateChangeCallback(s->peer, s_gathering) < 0 ||
        rtcSetStateChangeCallback(s->peer, s_state) < 0 ||
        rtcSetRemoteDescription(s->peer, sdp, "offer") < 0 ||
        rtcSetLocalDescription(s->peer, "answer") < 0) {
        publish_answer(r, broker, client, "error offer", NULL);
        session_free(s, 0);
        free(copy);
        return;
    }
    free(copy);
    s->next = r->sessions;
    r->sessions = s;
    ++r->peers;
    relay_log(r, "web relay: offer from %s%s\n", client, NULL);
}

struct relay_message_context { struct touch_relay *relay; int broker; };
static void on_mqtt_message(void *p, const char *t, const unsigned char *payload, int size)
{
    struct relay_message_context *c = (struct relay_message_context *)p;
    char want[256];
    topic(c->relay, want, sizeof(want), "offer", c->relay->id);
    if (!strcmp(t, want)) handle_offer(c->relay, c->broker, payload, size);
}

static void random_id(char *out)
{
    unsigned char bytes[8];
    FILE *f = fopen("/dev/urandom", "rb");
    int i;
    if (!f || fread(bytes, 1, sizeof(bytes), f) != sizeof(bytes)) {
        unsigned long seed = (unsigned long)time(NULL) ^ (unsigned long)(size_t)out;
        for (i = 0; i < 8; ++i) { seed = seed * 6364136223846793005UL + 1; bytes[i] = (unsigned char)(seed >> 33); }
    }
    if (f) fclose(f);
    for (i = 0; i < 8; ++i) sprintf(out + i * 2, "%02x", bytes[i]);
    out[16] = 0;
}

struct touch_relay *Touch_RelayCreate(const struct touch_relay_config *config)
{
    struct touch_relay *r = (struct touch_relay *)calloc(1, sizeof(*r));
    const char *p;
    if (!r) return NULL;
    r->config = *config;
    if (!r->config.offers_per_minute) r->config.offers_per_minute = 30;
    if (!r->config.connect_timeout) r->config.connect_timeout = 20;
    r->brokers_text = strdup(config->brokers ? config->brokers : "");
    r->prefix = strdup(config->topic_prefix && *config->topic_prefix ? config->topic_prefix : RELAY_DEFAULT_PREFIX);
    r->masters = strdup(config->masters ? config->masters : "");
    r->stun = strdup(config->stun ? config->stun : "");
    if (!r->brokers_text || !r->prefix || !r->masters || !r->stun) { Touch_RelayDestroy(r); return NULL; }
    r->config.brokers = r->config.topic_prefix = r->config.masters = r->config.stun = NULL;
    random_id(r->id);
    r->tokens = r->config.offers_per_minute;
    r->tokens_at = relay_now();
    Touch_BridgeSetIceServer(*r->stun ? r->stun : NULL);
    for (p = r->brokers_text; *p && r->broker_count < RELAY_MAX_BROKERS;) {
        size_t n;
        while (*p == ' ') ++p;
        n = strcspn(p, " ");
        if (n) {
            char *url = (char *)malloc(n + 1);
            if (!url) break;
            memcpy(url, p, n);
            url[n] = 0;
            r->brokers[r->broker_count++].url = url;
        }
        p += n;
    }
    if (!r->broker_count) { Touch_RelayDestroy(r); return NULL; }
    return r;
}

void Touch_RelayDestroy(struct touch_relay *r)
{
    int i;
    if (!r) return;
    while (r->sessions) {
        struct relay_session *s = r->sessions;
        r->sessions = s->next;
        session_free(s, 0);
    }
    for (i = 0; i < r->broker_count; ++i) {
        struct relay_broker *b = &r->brokers[i];
        if (b->mqtt) {
            if (b->ready) {
                char t[256];
                topic(r, t, sizeof(t), "online", r->id);
                Touch_MqttPublish(b->mqtt, t, "", 0, 1);
            }
            Touch_MqttDestroy(b->mqtt);
        }
        free(b->url);
    }
    free(r->brokers_text); free(r->prefix); free(r->masters); free(r->stun);
    free(r);
}

static void broker_frame(struct touch_relay *r, int index, double now)
{
    struct relay_broker *b = &r->brokers[index];
    struct relay_message_context c = {r, index};
    int state;
    if (!b->mqtt) {
        char client[32], will[256];
        if (now < b->retry_at) return;
        snprintf(client, sizeof(client), "gnr-%s", r->id);
        topic(r, will, sizeof(will), "online", r->id);
        b->mqtt = Touch_MqttCreate(b->url, client, will, 60);
        b->ready = 0;
        if (!b->mqtt) { b->backoff = b->backoff ? b->backoff * 2 : 5; b->retry_at = now + b->backoff; return; }
    }
    state = Touch_MqttPoll(b->mqtt, on_mqtt_message, &c);
    if (state == TOUCH_MQTT_FAILED) {
        if (b->ready) relay_log(r, "web relay: lost signaling broker %s%s\n", b->url, NULL);
        Touch_MqttDestroy(b->mqtt);
        b->mqtt = NULL;
        b->ready = 0;
        b->backoff = b->backoff ? (b->backoff < 60 ? b->backoff * 2 : 120) : 5;
        b->retry_at = now + b->backoff;
        return;
    }
    if (state == TOUCH_MQTT_CONNECTED && !b->ready) {
        char t[256];
        topic(r, t, sizeof(t), "offer", r->id);
        if (Touch_MqttSubscribe(b->mqtt, t) == 0) {
            b->ready = 1;
            b->backoff = 0;
            publish_presence(r, b);
            relay_log(r, "web relay %s online on %s\n", r->id, b->url);
        }
    } else if (b->ready && b->announced != r->peers) publish_presence(r, b);
}

void Touch_RelayFrame(struct touch_relay *r)
{
    struct relay_session **link = &r->sessions;
    double now = relay_now();
    int i;
    for (i = 0; i < r->broker_count; ++i) broker_frame(r, i, now);
    while (*link) {
        struct relay_session *s = *link;
        int result, drop;
        if (!s->answered && atomic_load(&s->gathered)) {
            int n = rtcGetLocalDescription(s->peer, NULL, 0);
            char *sdp = n > 0 && n <= RELAY_MAX_SDP ? (char *)malloc((size_t)n + 1) : NULL;
            if (sdp && rtcGetLocalDescription(s->peer, sdp, n + 1) >= 0) publish_answer(r, s->broker, s->client, NULL, sdp);
            else atomic_store(&s->failed, 1);
            free(sdp);
            s->answered = 1;
        }
        result = Touch_BridgePollPeer(s->forwarder, s->peer, atomic_load(&s->channel));
        drop = result == RTC_ERR_NOT_AVAIL || atomic_load(&s->failed) ||
               (!atomic_load(&s->opened) && now - s->created > r->config.connect_timeout);
        if (drop) {
            *link = s->next;
            --r->peers;
            relay_log(r, "web relay: session %s closed%s\n", s->client, NULL);
            session_free(s, result == RTC_ERR_NOT_AVAIL);
        } else link = &s->next;
    }
}

const char *Touch_RelayId(const struct touch_relay *r) { return r->id; }
int Touch_RelayPeers(const struct touch_relay *r) { return r->peers; }
int Touch_RelayBrokersOnline(const struct touch_relay *r)
{
    int i, n = 0;
    for (i = 0; i < r->broker_count; ++i) n += r->brokers[i].ready;
    return n;
}
