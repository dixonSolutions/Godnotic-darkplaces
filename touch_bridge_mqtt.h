#ifndef TOUCH_BRIDGE_MQTT_H
#define TOUCH_BRIDGE_MQTT_H

/* Minimal MQTT 3.1.1 client (QoS 0 only) over a libdatachannel WebSocket,
 * used for the web relay's public signaling. It does not depend on the
 * engine. All functions run on the owner thread; the WebSocket callbacks only
 * append to a locked buffer. */

struct touch_mqtt;

enum touch_mqtt_state {
    TOUCH_MQTT_FAILED = -1,     /* closed or refused: destroy it, maybe retry */
    TOUCH_MQTT_CONNECTING = 0,  /* WebSocket or MQTT handshake under way */
    TOUCH_MQTT_CONNECTED = 1    /* CONNACK accepted */
};

/* will_topic, when not NULL, gets an empty retained message from the broker
 * if this client disappears without DISCONNECT: it clears a presence. */
struct touch_mqtt *Touch_MqttCreate(const char *url, const char *client_id,
                                    const char *will_topic, int keepalive_seconds);
/* Sends DISCONNECT when connected, then closes and frees. */
void Touch_MqttDestroy(struct touch_mqtt *m);
int Touch_MqttSubscribe(struct touch_mqtt *m, const char *topic_filter);
int Touch_MqttPublish(struct touch_mqtt *m, const char *topic, const void *payload, int size, int retain);

typedef void (*touch_mqtt_message_fn)(void *context, const char *topic,
                                      const unsigned char *payload, int size);
/* Drives the handshake and keepalive and delivers received PUBLISH packets.
 * Returns the state after polling. */
int Touch_MqttPoll(struct touch_mqtt *m, touch_mqtt_message_fn message, void *context);

/* Packet encoders, exposed for tests. Return the packet size or -1. */
int Touch_MqttEncodeConnect(unsigned char *out, int capacity, const char *client_id,
                            const char *will_topic, int keepalive_seconds);
int Touch_MqttEncodePublish(unsigned char *out, int capacity, const char *topic,
                            const void *payload, int size, int retain);
int Touch_MqttEncodeSubscribe(unsigned char *out, int capacity, unsigned short packet_id,
                              const char *topic_filter);
/* Splits one packet off the front of a byte stream: 0 = need more bytes,
 * -1 = malformed, otherwise the whole packet's size. */
int Touch_MqttPacketSize(const unsigned char *data, int size, int *header_size);

#endif
