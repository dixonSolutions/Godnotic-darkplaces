/* Touch: engine side of the opt-in web relay (touch_bridge_relay.c): the
 * bridge_relay cvar ("Let web players connect through this device", default
 * off) starts it; it announces this device on public MQTT brokers and relays
 * browser players' datagrams with every stage 1b limit of touch_bridge.c. */
#include "quakedef.h"
#include "lhnet.h"
#include "touch_bridge.h"
#include "touch_bridge_relay.h"
#include <arpa/inet.h>
#include <string.h>

static cvar_t bridge_relay = {.flags = CF_SHARED | CF_ARCHIVE, .name = "bridge_relay", .string = "0",
    .description = "Let web players connect through this device: relay browser players' game traffic over WebRTC (opt-in)"};
static cvar_t bridge_relay_brokers = {.flags = CF_SHARED, .name = "bridge_relay_brokers",
    .string = "wss://broker.emqx.io:8084/mqtt wss://broker.hivemq.com:8884/mqtt",
    .description = "public MQTT-over-WebSocket brokers the web relay announces itself on (space-separated)"};
static cvar_t bridge_relay_stun = {.flags = CF_SHARED, .name = "bridge_relay_stun", .string = "stun:stun.l.google.com:19302",
    .description = "STUN server the web relay uses to find its public address (empty: none)"};
static cvar_t bridge_relay_topic = {.flags = CF_SHARED, .name = "bridge_relay_topic", .string = "godnotic/relay/v1",
    .description = "MQTT topic prefix of the web relay signaling"};
static cvar_t bridge_relay_status = {.flags = CF_SHARED | CF_READONLY, .name = "bridge_relay_status", .string = "off",
    .description = "web relay state: off, connecting, online N/M, failed"};

void Touch_BridgeRelayInit(void)
{
    Cvar_RegisterVariable(&bridge_relay);
    Cvar_RegisterVariable(&bridge_relay_brokers);
    Cvar_RegisterVariable(&bridge_relay_stun);
    Cvar_RegisterVariable(&bridge_relay_topic);
    Cvar_RegisterVariable(&bridge_relay_status);
}

/* ---- Opt-in web relay (touch_bridge_relay.c) ---- */
static struct touch_relay *bridge_relay_state;
static char bridge_relay_settings[2048];

static struct touch_bridge_forwarder *bridge_relay_forwarder(void *context)
{
    (void)context;
    return Touch_BridgeCreateConfiguredForwarder();
}
static void bridge_relay_log(void *context, const char *text)
{
    (void)context;
    Con_Printf("%s", text);
}
/* "ip:port ..." of the configured masters, for browsers' sv_masterextra. */
static void bridge_relay_masters(char *out, size_t size)
{
    size_t i, at = 0;
    out[0] = 0;
    for (i = 0; i < TOUCH_BRIDGE_MASTER_CVARS; ++i) {
        struct touch_bridge_address a;
        char ip[64];
        if (!Touch_BridgeConfiguredMaster(i, &a)) continue;
        if (!inet_ntop(a.family == 4 ? AF_INET : AF_INET6, a.address, ip, sizeof(ip))) continue;
        at += (size_t)dpsnprintf(out + at, size - at, a.family == 4 ? "%s%s:%u" : "%s[%s]:%u",
                                 at ? " " : "", ip, (unsigned)a.port);
        if (at >= size - 1) { out[0] = 0; return; }
    }
}

static void bridge_relay_stop(void)
{
    if (!bridge_relay_state) return;
    Touch_RelayDestroy(bridge_relay_state);
    bridge_relay_state = NULL;
    Con_Printf("web relay: off\n");
}

void Touch_BridgeRelayFrame(void)
{
    char settings[sizeof(bridge_relay_settings)], status[64];
    dpsnprintf(settings, sizeof(settings), "%s|%s|%s|%s|%s", bridge_relay.string, bridge_relay_brokers.string,
               bridge_relay_stun.string, bridge_relay_topic.string, Cvar_VariableString(&cvars_all, "bridge_allow_loopback", CF_SERVER));
    if (bridge_relay_state && strcmp(settings, bridge_relay_settings)) bridge_relay_stop();
    if (!bridge_relay_state && bridge_relay.integer && strcmp(settings, bridge_relay_settings)) {
        struct touch_relay_config config;
        char masters[512];
        bridge_relay_masters(masters, sizeof(masters));
        memset(&config, 0, sizeof(config));
        config.brokers = bridge_relay_brokers.string;
        config.topic_prefix = bridge_relay_topic.string;
        config.masters = masters;
        config.stun = bridge_relay_stun.string;
        config.make_forwarder = bridge_relay_forwarder;
        config.log = bridge_relay_log;
        bridge_relay_state = Touch_RelayCreate(&config);
        Con_Printf(bridge_relay_state ? "web relay: starting (masters: %s)\n" : "web relay: cannot start%s\n",
                   bridge_relay_state ? masters : "");
    }
    dp_strlcpy(bridge_relay_settings, settings, sizeof(bridge_relay_settings));
    if (bridge_relay_state) Touch_RelayFrame(bridge_relay_state);
    if (!bridge_relay.integer) dp_strlcpy(status, "off", sizeof(status));
    else if (!bridge_relay_state) dp_strlcpy(status, "failed", sizeof(status));
    else if (!Touch_RelayBrokersOnline(bridge_relay_state)) dp_strlcpy(status, "connecting", sizeof(status));
    else dpsnprintf(status, sizeof(status), "online %d/%u", Touch_RelayPeers(bridge_relay_state), Touch_BridgeGetMaxPeers());
    if (strcmp(status, bridge_relay_status.string)) Cvar_SetQuick(&bridge_relay_status, status);
}

void Touch_BridgeRelayShutdown(void)
{
    bridge_relay_stop();
}
