/* Engine-side forwarder construction. Call on the engine thread after cvars
 * have been loaded; each peer receives a snapshot of the configured masters. */
#include "quakedef.h"
#include "lhnet.h"
#include "touch_bridge.h"
#include <netinet/in.h>
#include <string.h>

static cvar_t bridge_allow_loopback = {.flags = CF_SERVER, .name = "bridge_allow_loopback", .string = "0",
    .description = "Test only: allow the WebRTC bridge to send UDP to 127.0.0.1 on any port"};

void Touch_BridgeInit(void)
{
    Cvar_RegisterVariable(&bridge_allow_loopback);
}

static const char *const bridge_master_cvars[TOUCH_BRIDGE_MASTER_CVARS] = {
    "sv_master1", "sv_master2", "sv_master3", "sv_master4",
    "sv_masterextra1", "sv_masterextra2", "sv_masterextra3"
};

/* Resolves configured master number i; 0 when unset or unresolvable. */
int Touch_BridgeConfiguredMaster(size_t i, struct touch_bridge_address *address)
{
    if (i >= TOUCH_BRIDGE_MASTER_CVARS) return 0;
    {
    const char *value = Cvar_VariableString(&cvars_all, bridge_master_cvars[i], CF_SERVER);
    lhnetaddress_t resolved;
    if (!value || !*value || !LHNETADDRESS_FromString(&resolved, value, 27950)) return 0;
    memset(address, 0, sizeof(*address));
    if (resolved.addresstype == LHNETADDRESSTYPE_INET4) {
        struct sockaddr_in native;
        memcpy(&native, resolved.storage, sizeof(native));
        address->family = 4;
        memcpy(address->address, &native.sin_addr, 4);
    } else if (resolved.addresstype == LHNETADDRESSTYPE_INET6) {
        struct sockaddr_in6 native;
        memcpy(&native, resolved.storage, sizeof(native));
        address->family = 6;
        memcpy(address->address, &native.sin6_addr, 16);
    } else return 0;
    address->port = (unsigned short)LHNETADDRESS_GetPort(&resolved);
    return 1;
    }
}

struct touch_bridge_forwarder *Touch_BridgeCreateConfiguredForwarder(void)
{
    struct touch_bridge_forwarder *f = Touch_BridgeForwarderCreate();
    size_t i;
    if (!f) return NULL;
    Touch_BridgeSetAllowLoopback(f, bridge_allow_loopback.integer);
    for (i = 0; i < sizeof(bridge_master_cvars) / sizeof(bridge_master_cvars[0]); ++i) {
        struct touch_bridge_address address;
        if (!Touch_BridgeConfiguredMaster(i, &address)) continue;
        if (Touch_BridgeAllowMaster(f, &address) < 0) {
            Touch_BridgeForwarderDestroy(f);
            return NULL;
        }
    }
    return f;
}
