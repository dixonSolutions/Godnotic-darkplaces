/* Minimal MQTT 3.1.1 (QoS 0) over a libdatachannel WebSocket. See
 * touch_bridge_mqtt.h. Only what the relay signaling needs: CONNECT with an
 * empty retained will, SUBSCRIBE, PUBLISH, PINGREQ, DISCONNECT. */
#define _POSIX_C_SOURCE 200809L
#include "touch_bridge_mqtt.h"
#include <rtc/rtc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MQTT_MAX_INPUT (256 * 1024)
#define MQTT_MAX_PACKET (128 * 1024)

struct touch_mqtt {
    int ws;
    pthread_mutex_t lock;
    unsigned char *in;
    int in_len;
    atomic_int open, closed, overflow;
    int sent_connect, connected, failed;
    int keepalive;
    double started, last_send, ping_sent;
    unsigned short next_id;
    char *client_id, *will_topic;
};

static double mqtt_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static int put_length(unsigned char *out, int capacity, int length)
{
    int n = 0;
    do {
        unsigned char byte = (unsigned char)(length & 127);
        length >>= 7;
        if (length) byte |= 128;
        if (n >= capacity) return -1;
        out[n++] = byte;
    } while (length && n < 4);
    return length ? -1 : n;
}

static int put_string(unsigned char *out, int at, int capacity, const void *s, int size)
{
    if (size < 0 || size > 65535 || at + 2 + size > capacity) return -1;
    out[at] = (unsigned char)(size >> 8);
    out[at + 1] = (unsigned char)size;
    if (size) memcpy(out + at + 2, s, (size_t)size);
    return at + 2 + size;
}

/* Builds fixed header + body: body is written at out + 5 first, then moved. */
static int finish(unsigned char *out, int capacity, unsigned char type, int body)
{
    unsigned char header[5];
    int n;
    header[0] = type;
    n = put_length(header + 1, 4, body);
    if (n < 0 || 1 + n + body > capacity) return -1;
    memmove(out + 1 + n, out + 5, (size_t)body);
    memcpy(out, header, (size_t)(1 + n));
    return 1 + n + body;
}

int Touch_MqttEncodeConnect(unsigned char *out, int capacity, const char *client_id,
                            const char *will_topic, int keepalive)
{
    static const unsigned char proto[] = {0, 4, 'M', 'Q', 'T', 'T', 4};
    int at = 5;
    if (!client_id || capacity < 5 + 10 || keepalive < 0 || keepalive > 65535) return -1;
    memcpy(out + at, proto, sizeof(proto));
    at += sizeof(proto);
    /* clean session; will (QoS 0, retained, empty) when a topic is given */
    out[at++] = (unsigned char)(0x02 | (will_topic ? 0x04 | 0x20 : 0));
    out[at++] = (unsigned char)(keepalive >> 8);
    out[at++] = (unsigned char)keepalive;
    at = put_string(out, at, capacity, client_id, (int)strlen(client_id));
    if (at < 0) return -1;
    if (will_topic) {
        at = put_string(out, at, capacity, will_topic, (int)strlen(will_topic));
        if (at < 0) return -1;
        at = put_string(out, at, capacity, "", 0);
        if (at < 0) return -1;
    }
    return finish(out, capacity, 0x10, at - 5);
}

int Touch_MqttEncodePublish(unsigned char *out, int capacity, const char *topic,
                            const void *payload, int size, int retain)
{
    int at;
    if (!topic || size < 0 || capacity < 5) return -1;
    at = put_string(out, 5, capacity, topic, (int)strlen(topic));
    if (at < 0 || at + size > capacity) return -1;
    if (size) memcpy(out + at, payload, (size_t)size);
    return finish(out, capacity, (unsigned char)(0x30 | (retain ? 1 : 0)), at + size - 5);
}

int Touch_MqttEncodeSubscribe(unsigned char *out, int capacity, unsigned short id,
                              const char *filter)
{
    int at = 5;
    if (!filter || !id || capacity < 8) return -1;
    out[at++] = (unsigned char)(id >> 8);
    out[at++] = (unsigned char)id;
    at = put_string(out, at, capacity, filter, (int)strlen(filter));
    if (at < 0 || at >= capacity) return -1;
    out[at++] = 0; /* QoS 0 */
    return finish(out, capacity, 0x82, at - 5);
}

int Touch_MqttPacketSize(const unsigned char *data, int size, int *header_size)
{
    int length = 0, shift = 0, i;
    for (i = 1; i < 5; ++i) {
        if (i >= size) return 0;
        length |= (data[i] & 127) << shift;
        shift += 7;
        if (!(data[i] & 128)) break;
    }
    if (i == 5) return -1;
    if (length > MQTT_MAX_PACKET) return -1;
    if (header_size) *header_size = i + 1;
    return size >= i + 1 + length ? i + 1 + length : 0;
}

static void on_open(int ws, void *p)
{
    struct touch_mqtt *m = (struct touch_mqtt *)p;
    (void)ws;
    atomic_store(&m->open, 1);
}
static void on_closed(int ws, void *p)
{
    struct touch_mqtt *m = (struct touch_mqtt *)p;
    (void)ws;
    atomic_store(&m->closed, 1);
}
static void on_error(int ws, const char *error, void *p)
{
    (void)error;
    on_closed(ws, p);
}
static void on_message(int ws, const char *data, int size, void *p)
{
    struct touch_mqtt *m = (struct touch_mqtt *)p;
    (void)ws;
    if (size < 0) return; /* text frames are not MQTT */
    pthread_mutex_lock(&m->lock);
    if (m->in_len + size > MQTT_MAX_INPUT) atomic_store(&m->overflow, 1);
    else {
        memcpy(m->in + m->in_len, data, (size_t)size);
        m->in_len += size;
    }
    pthread_mutex_unlock(&m->lock);
}

static int mqtt_send(struct touch_mqtt *m, const unsigned char *packet, int size)
{
    if (size < 0 || m->ws < 0 || rtcSendMessage(m->ws, (const char *)packet, size) < 0) return -1;
    m->last_send = mqtt_now();
    return 0;
}

struct touch_mqtt *Touch_MqttCreate(const char *url, const char *client_id,
                                    const char *will_topic, int keepalive)
{
    static const char *protocols[] = {"mqtt"};
    rtcWsConfiguration config;
    struct touch_mqtt *m = (struct touch_mqtt *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(&m->lock, NULL);
    m->in = (unsigned char *)malloc(MQTT_MAX_INPUT);
    m->client_id = strdup(client_id);
    m->will_topic = will_topic ? strdup(will_topic) : NULL;
    m->keepalive = keepalive > 0 ? keepalive : 60;
    m->next_id = 1;
    m->started = mqtt_now();
    m->ws = -1;
    if (!m->in || !m->client_id || (will_topic && !m->will_topic)) { Touch_MqttDestroy(m); return NULL; }
    memset(&config, 0, sizeof(config));
    config.protocols = protocols;
    config.protocolsCount = 1;
    config.connectionTimeoutMs = 10000;
    config.maxMessageSize = MQTT_MAX_PACKET + 16;
    m->ws = rtcCreateWebSocketEx(url, &config);
    if (m->ws < 0) { Touch_MqttDestroy(m); return NULL; }
    rtcSetUserPointer(m->ws, m);
    if (rtcSetOpenCallback(m->ws, on_open) < 0 || rtcSetClosedCallback(m->ws, on_closed) < 0 ||
        rtcSetErrorCallback(m->ws, on_error) < 0 || rtcSetMessageCallback(m->ws, on_message) < 0) {
        Touch_MqttDestroy(m);
        return NULL;
    }
    return m;
}

void Touch_MqttDestroy(struct touch_mqtt *m)
{
    if (!m) return;
    if (m->ws >= 0) {
        if (m->connected && !atomic_load(&m->closed)) {
            static const unsigned char disconnect[] = {0xE0, 0};
            rtcSendMessage(m->ws, (const char *)disconnect, 2);
        }
        rtcDeleteWebSocket(m->ws); /* waits for callbacks to finish */
    }
    pthread_mutex_destroy(&m->lock);
    free(m->in);
    free(m->client_id);
    free(m->will_topic);
    free(m);
}

int Touch_MqttSubscribe(struct touch_mqtt *m, const char *filter)
{
    unsigned char packet[1024];
    if (!m->connected) return -1;
    if (!++m->next_id) m->next_id = 1;
    return mqtt_send(m, packet, Touch_MqttEncodeSubscribe(packet, sizeof(packet), m->next_id, filter));
}

int Touch_MqttPublish(struct touch_mqtt *m, const char *topic, const void *payload, int size, int retain)
{
    unsigned char *packet;
    int n, result;
    if (!m->connected || size < 0 || size > MQTT_MAX_PACKET - 1024) return -1;
    packet = (unsigned char *)malloc((size_t)size + 1024);
    if (!packet) return -1;
    n = Touch_MqttEncodePublish(packet, size + 1024, topic, payload, size, retain);
    result = mqtt_send(m, packet, n);
    free(packet);
    return result;
}

static int handle_packet(struct touch_mqtt *m, const unsigned char *p, int size, int header,
                         touch_mqtt_message_fn message, void *context)
{
    unsigned char type = p[0] >> 4;
    const unsigned char *body = p + header;
    int length = size - header;
    if (type == 2) { /* CONNACK */
        if (length != 2 || body[1] != 0) return -1;
        m->connected = 1;
    } else if (type == 3) { /* PUBLISH */
        int qos = (p[0] >> 1) & 3, topic_len, at;
        char topic[512];
        if (!m->connected || length < 2) return -1;
        topic_len = (body[0] << 8) | body[1];
        at = 2 + topic_len + (qos ? 2 : 0);
        if (topic_len >= (int)sizeof(topic) || at > length) return -1;
        memcpy(topic, body + 2, (size_t)topic_len);
        topic[topic_len] = 0;
        if (qos == 1) { /* acknowledge, though QoS 0 is all we ask for */
            unsigned char ack[4] = {0x40, 2, body[2 + topic_len], body[3 + topic_len]};
            mqtt_send(m, ack, 4);
        }
        if (message) message(context, topic, body + at, length - at);
    }
    /* SUBACK, PINGRESP and others need nothing beyond counting as traffic. */
    m->ping_sent = 0;
    return 0;
}

int Touch_MqttPoll(struct touch_mqtt *m, touch_mqtt_message_fn message, void *context)
{
    double now = mqtt_now();
    if (m->failed) return TOUCH_MQTT_FAILED;
    if (atomic_load(&m->open) && !m->sent_connect) {
        unsigned char packet[1024];
        m->sent_connect = 1;
        if (mqtt_send(m, packet, Touch_MqttEncodeConnect(packet, sizeof(packet), m->client_id,
                                                         m->will_topic, m->keepalive)) < 0)
            m->failed = 1;
    }
    for (;;) {
        unsigned char *packet = NULL;
        int size, header = 0;
        pthread_mutex_lock(&m->lock);
        size = Touch_MqttPacketSize(m->in, m->in_len, &header);
        if (size > 0) {
            packet = (unsigned char *)malloc((size_t)size);
            if (packet) {
                memcpy(packet, m->in, (size_t)size);
                memmove(m->in, m->in + size, (size_t)(m->in_len - size));
                m->in_len -= size;
            }
        }
        pthread_mutex_unlock(&m->lock);
        if (size < 0 || (size > 0 && !packet)) { m->failed = 1; break; }
        if (!size) break;
        if (handle_packet(m, packet, size, header, message, context) < 0) m->failed = 1;
        free(packet);
        if (m->failed) break;
    }
    if (atomic_load(&m->closed) || atomic_load(&m->overflow)) m->failed = 1;
    if (!m->connected && now - m->started > 15) m->failed = 1;
    if (m->connected && !m->failed) {
        if (m->ping_sent && now - m->ping_sent > m->keepalive) m->failed = 1;
        else if (!m->ping_sent && now - m->last_send > m->keepalive / 2.0) {
            static const unsigned char ping[] = {0xC0, 0};
            if (mqtt_send(m, ping, 2) < 0) m->failed = 1;
            m->ping_sent = now;
        }
    }
    return m->failed ? TOUCH_MQTT_FAILED : m->connected ? TOUCH_MQTT_CONNECTED : TOUCH_MQTT_CONNECTING;
}
