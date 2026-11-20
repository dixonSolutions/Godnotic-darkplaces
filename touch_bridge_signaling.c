/* Manual, non-trickle SDP signaling. Engine APIs and session list stay on the
 * engine thread; libdatachannel callbacks communicate through atomics. */
#define _POSIX_C_SOURCE 200809L
#include "quakedef.h"
#include "touch_bridge.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BRIDGE_SDP_MAX (1024 * 1024)
struct bridge_session {
    struct bridge_session *next;
    struct touch_bridge_forwarder *forwarder;
    int peer;
    atomic_int channel, gathered, failed;
    char *answer_path;
};
static struct bridge_session *sessions;

static void description(int pc, const char *sdp, const char *type, void *context)
{ (void)pc; (void)sdp; (void)type; (void)context; }
static void candidate(int pc, const char *sdp, const char *mid, void *context)
{ (void)pc; (void)sdp; (void)mid; (void)context; }
static void opened(int channel, void *context) { (void)channel; (void)context; }
static void message(int channel, const char *data, int size, void *context)
{
    struct bridge_session *s = (struct bridge_session *)context;
    Touch_BridgeForwardMessage(channel, data, size, s->forwarder);
}
static void channel_received(int pc, int channel, void *context)
{
    struct bridge_session *s = (struct bridge_session *)context;
    int expected = -1;
    (void)pc;
    if (!atomic_compare_exchange_strong(&s->channel, &expected, channel)) {
        rtcDeleteDataChannel(channel);
        return;
    }
    if (Touch_BridgeAcceptChannel(channel, s, opened, message) < 0)
        atomic_store(&s->failed, 1);
}
static void gathering(int pc, rtcGatheringState state, void *context)
{
    struct bridge_session *s = (struct bridge_session *)context;
    (void)pc;
    if (state == RTC_GATHERING_COMPLETE) atomic_store(&s->gathered, 1);
}
static void state_changed(int pc, rtcState state, void *context)
{
    struct bridge_session *s = (struct bridge_session *)context;
    (void)pc;
    if (state == RTC_FAILED || state == RTC_CLOSED) atomic_store(&s->failed, 1);
}
static void destroy_session(struct bridge_session *s, int peer_deleted)
{
    int channel;
    if (!peer_deleted) Touch_BridgeDeletePeer(s->peer);
    channel = atomic_load(&s->channel);
    if (channel >= 0) rtcDeleteDataChannel(channel);
    Touch_BridgeForwarderDestroy(s->forwarder);
    free(s->answer_path);
    free(s);
}
static char *read_offer(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *data;
    size_t n;
    if (!f) return NULL;
    data = (char *)malloc(BRIDGE_SDP_MAX + 1);
    if (!data) { fclose(f); return NULL; }
    n = fread(data, 1, BRIDGE_SDP_MAX + 1, f);
    if (ferror(f) || !n || n > BRIDGE_SDP_MAX || memchr(data, 0, n)) {
        free(data); fclose(f); return NULL;
    }
    data[n] = 0;
    fclose(f);
    return data;
}
static int write_answer(struct bridge_session *s)
{
    int n = rtcGetLocalDescription(s->peer, NULL, 0), fd, ok = 0;
    char *sdp, *tmp;
    FILE *f;
    if (n <= 0 || n > BRIDGE_SDP_MAX) return -1;
    sdp = (char *)malloc((size_t)n + 1);
    tmp = (char *)malloc(strlen(s->answer_path) + 12);
    if (!sdp || !tmp) { free(sdp); free(tmp); return -1; }
    if (rtcGetLocalDescription(s->peer, sdp, n + 1) < 0) goto done;
    memcpy(tmp, s->answer_path, strlen(s->answer_path));
    memcpy(tmp + strlen(s->answer_path), ".tmpXXXXXX", 11);
    fd = mkstemp(tmp);
    if (fd < 0) goto done;
    f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp); goto done; }
    n = (int)strlen(sdp);
    if (fwrite(sdp, 1, (size_t)n, f) == (size_t)n && !fflush(f)) ok = 1;
    if (fclose(f)) ok = 0;
    if (!ok || rename(tmp, s->answer_path)) { unlink(tmp); ok = 0; }
done:
    free(sdp); free(tmp);
    return ok ? 0 : -1;
}
static void bridge_offer(cmd_state_t *cmd)
{
    struct bridge_session *s;
    char *offer;
    if (Cmd_Argc(cmd) != 3) {
        Con_Printf("usage: bridge_offer <offer.sdp> <answer.sdp>\n"); return;
    }
    offer = read_offer(Cmd_Argv(cmd, 1));
    if (!offer) { Con_Printf("bridge: cannot read offer\n"); return; }
    s = (struct bridge_session *)calloc(1, sizeof(*s));
    if (!s) { free(offer); return; }
    atomic_init(&s->channel, -1); atomic_init(&s->gathered, 0); atomic_init(&s->failed, 0);
    s->answer_path = strdup(Cmd_Argv(cmd, 2));
    s->forwarder = Touch_BridgeCreateConfiguredForwarder();
    s->peer = -1;
    if (s->answer_path && s->forwarder)
        s->peer = Touch_BridgeCreatePeer(s, description, candidate, channel_received);
    if (s->peer < 0) {
        Con_Printf("bridge: cannot create peer\n");
        if (s->forwarder) Touch_BridgeForwarderDestroy(s->forwarder);
        free(s->answer_path); free(s); free(offer); return;
    }
    if (rtcSetGatheringStateChangeCallback(s->peer, gathering) < 0 ||
        rtcSetStateChangeCallback(s->peer, state_changed) < 0 ||
        rtcSetRemoteDescription(s->peer, offer, "offer") < 0 ||
        rtcSetLocalDescription(s->peer, "answer") < 0) {
        Con_Printf("bridge: invalid offer or signaling failure\n");
        free(offer); destroy_session(s, 0); return;
    }
    free(offer);
    s->next = sessions; sessions = s;
    Con_Printf("bridge: gathering answer for %s\n", s->answer_path);
}
void Touch_BridgeSignalingInit(void)
{
    Touch_BridgeRelayInit();
    Cmd_AddCommand(CF_SERVER, "bridge_offer", bridge_offer, "accept raw SDP offer file and write a complete SDP answer file");
}
void Touch_BridgeSignalingFrame(void)
{
    struct bridge_session **link = &sessions;
    Touch_BridgeRelayFrame();
    while (*link) {
        struct bridge_session *s = *link;
        int result;
        if (s->answer_path && atomic_load(&s->gathered)) {
            if (write_answer(s) < 0) {
                Con_Printf("bridge: cannot write answer to %s\n", s->answer_path);
                atomic_store(&s->failed, 1);
            } else Con_Printf("bridge: answer written to %s\n", s->answer_path);
            free(s->answer_path); s->answer_path = NULL;
        }
        result = Touch_BridgePollPeer(s->forwarder, s->peer, atomic_load(&s->channel));
        if (result == RTC_ERR_NOT_AVAIL || atomic_load(&s->failed)) {
            *link = s->next;
            destroy_session(s, result == RTC_ERR_NOT_AVAIL);
        } else link = &s->next;
    }
}
void Touch_BridgeSignalingShutdown(void)
{
    Touch_BridgeRelayShutdown();
    while (sessions) {
        struct bridge_session *s = sessions;
        sessions = s->next;
        destroy_session(s, 0);
    }
}
