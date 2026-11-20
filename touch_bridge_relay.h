#ifndef TOUCH_BRIDGE_RELAY_H
#define TOUCH_BRIDGE_RELAY_H

/* The opt-in web relay: announces itself on public MQTT brokers, answers the
 * WebRTC offers browsers send there, and forwards each browser's datagrams
 * with its own touch_bridge forwarder (all stage 1b limits apply). Shared by
 * the engine (bridge_relay cvar) and the standalone godnotic-relay. Not
 * dependent on the engine; call everything from one thread.
 *
 * Signaling messages (UTF-8 text, header lines, a blank line, then a body):
 *   <prefix>/online/<relay>   retained "GODNOTIC-RELAY 1\npeers N\nmax M\n\n";
 *                             cleared (empty retained) by a will or on stop
 *   <prefix>/offer/<relay>    "GODNOTIC-OFFER 1\nfrom <client>\n\n<SDP offer>"
 *   <prefix>/answer/<client>  "GODNOTIC-ANSWER 1\nrelay <relay>\nmaster <ip:port>...\n\n<SDP>"
 *                             or "GODNOTIC-ANSWER 1\nrelay <relay>\nerror <reason>\n\n" */

struct touch_bridge_forwarder;
struct touch_relay;

struct touch_relay_config {
    const char *brokers;        /* space-separated ws:// or wss:// MQTT URLs */
    const char *topic_prefix;   /* default "godnotic/relay/v1" */
    const char *masters;        /* "ip:port ip:port ...": masters browsers should ask */
    const char *stun;           /* "stun:host:port" or NULL / "" for none */
    unsigned int offers_per_minute; /* default 30 */
    unsigned int connect_timeout;   /* seconds for an answered offer to open; default 20 */
    struct touch_bridge_forwarder *(*make_forwarder)(void *context);
    void (*log)(void *context, const char *text);
    void *context;
};

struct touch_relay *Touch_RelayCreate(const struct touch_relay_config *config);
/* Clears the presence, closes every session and broker connection. */
void Touch_RelayDestroy(struct touch_relay *relay);
void Touch_RelayFrame(struct touch_relay *relay);
const char *Touch_RelayId(const struct touch_relay *relay);
int Touch_RelayPeers(const struct touch_relay *relay);
int Touch_RelayBrokersOnline(const struct touch_relay *relay);

/* Message parsing, exposed for tests. Returns 0 and the client id copied to
 * client (at least 65 bytes), sdp pointing into text; -1 when malformed. */
int Touch_RelayParseOffer(const char *text, int size, char *client, const char **sdp);

#endif
