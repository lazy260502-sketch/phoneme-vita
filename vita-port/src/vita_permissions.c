/*
 * vita_permissions.c - MIDP permission policy for the PS Vita port (v02.90)
 *
 * Roadmap P0-6. Replaces phoneME's generic_policy_load.o (removed from
 * libobj.a by CMake, the same `ar d` mechanism used for anc_st_vibrate.o
 * and lfjord_fb_export.o) and provides its eight symbols.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Upstream's hard-coded policy gives an *unidentified third party* suite
 * (which is what an unsigned jar is) these defaults:
 *
 *     net_access                SESSION / ONESHOT   -> prompt
 *     read_user_data_access     ONESHOT             -> prompt
 *     write_user_data_access    ONESHOT             -> prompt
 *     multimedia_recording      SESSION             -> prompt
 *
 * A prompt means MIDP blocks the calling MIDlet thread until the user
 * answers a modal dialog. This port runs exactly one MIDlet with no AMS,
 * and the permission dialog is a small LCDUI screen most games never
 * expect; the practical outcome is a frozen game (the roadmap's
 * "权限弹窗卡住游戏"), or a SecurityException on the very first network or
 * file call.
 *
 * So the default here is RELAXED: every group is simply allowed, for every
 * domain. That is also what most J2ME emulators do (J2ME Loader, FreeJ2ME
 * and KEmulator all run unsigned jars with full permissions) - a J2ME game
 * is a self-contained toy, not an app with a privacy budget.
 *
 * STRICT mode restores the exact upstream tables, so a player who wants
 * spec behaviour (or wants to test a signed jar's real permissions) can
 * switch it on from 设置 > 权限; it is written to defaults.cfg as
 *     perms=strict
 * and read back here. Per-game override: game.cfg's perms=strict.
 *
 * WHAT IS NOT CHANGED
 * -------------------
 * - The domain list, group list and group member lists are byte-for-byte
 *   the upstream ones. They define the permission *identifiers*
 *   (Permissions.getId("javax.microedition.io.Connector.http") and friends),
 *   so changing them would make lookups throw SecurityException("Unknown
 *   permission"). Only the *values* differ.
 * - Permissions.MIDP and Permissions.AMS stay NEVER: they are forced to
 *   NEVER inside DomainPolicy.loadValues() anyway.
 * - Certificate validation / signature checks are untouched; under RELAXED
 *   they cannot fire, under STRICT they behave exactly as upstream.
 *
 * The tables below are copied from
 *   phoneme-midp/src/security/midp_permissions/reference/native/
 *   generic_policy_load.c            (GPL-2, Sun Microsystems)
 * which is the file this one replaces; the port as a whole is already a
 * derivative of that code, so no license situation changes.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <psp2/io/fcntl.h>
#include <psp2/types.h>

#include "vita_permissions.h"

#define PERMS_CFG   "ux0:/data/J2ME00001/defaults.cfg"
#define PERMS_LOG   "ux0:/data/J2ME00001/permissions_debug.log"

/* ------------------------------------------------------------------ */
/* Permission levels - values match upstream and Permissions.java      */
/* ------------------------------------------------------------------ */
#define P_NEVER    0
#define P_ALLOW    1
#define P_BLANKET  4
#define P_SESSION  8
#define P_ONESHOT 16

/* ------------------------------------------------------------------ */
/* Identifiers (must stay identical to upstream - see the header note) */
/* ------------------------------------------------------------------ */
typedef const char *vita_perm_string;

static vita_perm_string domainTBL[] = {
    "manufacturer",
    "operator",
    "identified_third_party",
    "unidentified_third_party,unsecured",
    "minimum,unsecured",
    "maximum,unsecured"
};
static const int domainTBLsize =
    (int)(sizeof(domainTBL) / sizeof(domainTBL[0]));

static vita_perm_string groupTBL[] = {
    "net_access",
    "low_level_net_access",
    "call_control",
    "application_auto_invocation",
    "local_connectivity",
    "messaging",
    "restricted_messaging",
    "multimedia_recording",
    "read_user_data_access",
    "write_user_data_access",
    "location",
    "landmark",
    "payment",
    "authentication",
    "smart_card",
    "satsa"
};
static const int groupTBLsize =
    (int)(sizeof(groupTBL) / sizeof(groupTBL[0]));

static vita_perm_string net_access_members[] = {
    "javax.microedition.io.Connector.http",
    "javax.microedition.io.Connector.https",
    "javax.microedition.io.Connector.obex.client.tcp",
    "javax.microedition.io.Connector.obex.server.tcp"
};

static vita_perm_string low_level_net_access_members[] = {
    "javax.microedition.io.Connector.datagram",
    "javax.microedition.io.Connector.datagramreceiver",
    "javax.microedition.io.Connector.socket",
    "javax.microedition.io.Connector.serversocket",
    "javax.microedition.io.Connector.ssl"
};

static vita_perm_string call_control_members[] = {
    "javax.microedition.io.Connector.sip",
    "javax.microedition.io.Connector.sips"
};

static vita_perm_string messaging_members[] = {
    "javax.wireless.messaging.sms.send",
    "javax.wireless.messaging.mms.send",
    "javax.microedition.io.Connector.sms",
    "javax.wireless.messaging.sms.receive",
    "javax.microedition.io.Connector.mms",
    "javax.wireless.messaging.mms.receive"
};

static vita_perm_string restricted_messaging_members[] = {
    "javax.wireless.messaging.cbs.receive",
    "javax.microedition.io.Connector.cbs"
};

static vita_perm_string application_auto_invocation_members[] = {
    "javax.microedition.io.PushRegistry",
    "javax.microedition.content.ContentHandler"
};

static vita_perm_string local_connectivity_members[] = {
    "javax.microedition.io.Connector.comm",
    "javax.microedition.io.Connector.obex.client",
    "javax.microedition.io.Connector.obex.server",
    "javax.microedition.io.Connector.bluetooth.client",
    "javax.microedition.io.Connector.bluetooth.server"
};

static vita_perm_string smart_card_members[] = {
    "javax.microedition.apdu.aid",
    "javax.microedition.jcrmi"
};

static vita_perm_string authentication_members[] = {
    "javax.microedition.securityservice.CMSMessageSignatureService"
};

static vita_perm_string multimedia_recording_members[] = {
    "javax.microedition.media.control.RecordControl",
    "javax.microedition.media.control.VideoControl.getSnapshot",
    "javax.microedition.amms.control.camera.enableShutterFeedback"
};

static vita_perm_string read_user_data_access_members[] = {
    "javax.microedition.pim.ContactList.read",
    "javax.microedition.pim.EventList.read",
    "javax.microedition.pim.ToDoList.read",
    "javax.microedition.io.Connector.file.read"
};

static vita_perm_string write_user_data_access_members[] = {
    "javax.microedition.pim.ContactList.write",
    "javax.microedition.pim.EventList.write",
    "javax.microedition.pim.ToDoList.write",
    "javax.microedition.io.Connector.file.write",
    "javax.microedition.amms.control.tuner.setPreset"
};

static vita_perm_string location_members[] = {
    "javax.microedition.location.Location",
    "javax.microedition.location.ProximityListener",
    "javax.microedition.location.Orientation"
};

static vita_perm_string landmark_members[] = {
    "javax.microedition.location.LandmarkStore.read",
    "javax.microedition.location.LandmarkStore.write",
    "javax.microedition.location.LandmarkStore.category",
    "javax.microedition.location.LandmarkStore.management"
};

static vita_perm_string payment_members[] = {
    "javax.microedition.payment.process"
};

static vita_perm_string satsa_members[] = {
    "javax.microedition.apdu.sat"
};

static struct _group_member {
    vita_perm_string *list;
    int size;
} group_membersTBL[] = {
    {net_access_members,
        sizeof(net_access_members) / sizeof(vita_perm_string)},
    {low_level_net_access_members,
        sizeof(low_level_net_access_members) / sizeof(vita_perm_string)},
    {call_control_members,
        sizeof(call_control_members) / sizeof(vita_perm_string)},
    {application_auto_invocation_members,
        sizeof(application_auto_invocation_members) / sizeof(vita_perm_string)},
    {local_connectivity_members,
        sizeof(local_connectivity_members) / sizeof(vita_perm_string)},
    {messaging_members,
        sizeof(messaging_members) / sizeof(vita_perm_string)},
    {restricted_messaging_members,
        sizeof(restricted_messaging_members) / sizeof(vita_perm_string)},
    {multimedia_recording_members,
        sizeof(multimedia_recording_members) / sizeof(vita_perm_string)},
    {read_user_data_access_members,
        sizeof(read_user_data_access_members) / sizeof(vita_perm_string)},
    {write_user_data_access_members,
        sizeof(write_user_data_access_members) / sizeof(vita_perm_string)},
    {location_members, sizeof(location_members) / sizeof(vita_perm_string)},
    {landmark_members, sizeof(landmark_members) / sizeof(vita_perm_string)},
    {payment_members, sizeof(payment_members) / sizeof(vita_perm_string)},
    {authentication_members,
        sizeof(authentication_members) / sizeof(vita_perm_string)},
    {smart_card_members, sizeof(smart_card_members) / sizeof(vita_perm_string)},
    {satsa_members, sizeof(satsa_members) / sizeof(vita_perm_string)}
};

/* ------------------------------------------------------------------ */
/* STRICT values - exactly upstream's tables                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int maxval;
    int defval;
} values;

static values identifiedTBL[] = {
    {P_BLANKET, P_SESSION},
    {P_BLANKET, P_SESSION},
    {P_BLANKET, P_ONESHOT},
    {P_BLANKET, P_ONESHOT},
    {P_BLANKET, P_SESSION},
    {P_BLANKET, P_ONESHOT},
    {P_BLANKET, P_ONESHOT},
    {P_BLANKET, P_SESSION},
    {P_BLANKET, P_ONESHOT},
    {P_BLANKET, P_ONESHOT},
    {P_BLANKET, P_SESSION},
    {P_BLANKET, P_SESSION},
    {P_ALLOW,   P_ALLOW},
    {P_BLANKET, P_SESSION},
    {P_BLANKET, P_SESSION},
    {P_NEVER,   P_NEVER}
};

static values unidentifiedTBL[] = {
    {P_SESSION, P_ONESHOT},
    {P_SESSION, P_ONESHOT},
    {P_ONESHOT, P_ONESHOT},
    {P_SESSION, P_ONESHOT},
    {P_BLANKET, P_ONESHOT},
    {P_ONESHOT, P_ONESHOT},
    {P_ONESHOT, P_ONESHOT},
    {P_SESSION, P_ONESHOT},
    {P_ONESHOT, P_ONESHOT},
    {P_ONESHOT, P_ONESHOT},
    {P_SESSION, P_ONESHOT},
    {P_SESSION, P_ONESHOT},
    {P_NEVER,   P_NEVER},
    {P_NEVER,   P_NEVER},
    {P_NEVER,   P_NEVER},
    {P_NEVER,   P_NEVER}
};

/* BLANKET (4) is a real MIDP level that Permissions.java also defines; it
 * appears only in the strict tables above. */

/* ------------------------------------------------------------------ */
/* Policy mode                                                        */
/* ------------------------------------------------------------------ */
/* -1 = not decided yet (lazy read of defaults.cfg on first query). */
static int g_mode = -1;
static int g_logged = 0;

static void perm_log(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    int n;
    SceUID fd;

    fd = sceIoOpen(PERMS_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                   0777);
    if (fd < 0) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        sceIoWrite(fd, buf, (SceSize)n);
    }
    sceIoClose(fd);
}

/* Read `perms=` out of defaults.cfg. Returns VITA_PERMS_RELAXED when the
 * file or the key is missing, which is the documented default. */
int vita_permissions_load(const char *path) {
    char buf[512];
    SceUID fd;
    int n;
    const char *p;
    int mode = VITA_PERMS_RELAXED;

    if (path == NULL) {
        path = PERMS_CFG;
    }
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return mode;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return mode;
    }
    buf[n] = '\0';
    for (p = buf; *p != '\0'; ) {
        const char *nl = strpbrk(p, "\r\n");
        const char *end = (nl != NULL) ? nl : p + strlen(p);
        long len = end - p;
        if (len > 6 && len < 64 && strncmp(p, "perms=", 6) == 0) {
            const char *v = p + 6;
            long vlen = len - 6;
            while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
            if (vlen >= 6 && strncmp(v, "strict", 6) == 0) {
                mode = VITA_PERMS_STRICT;
            } else if (vlen >= 7 && strncmp(v, "relaxed", 7) == 0) {
                mode = VITA_PERMS_RELAXED;
            }
            break;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return mode;
}

void vita_permissions_set_mode(int mode) {
    g_mode = (mode == VITA_PERMS_STRICT) ? VITA_PERMS_STRICT
                                         : VITA_PERMS_RELAXED;
    perm_log("[PERM] mode=%s (set by launcher)\n",
             (g_mode == VITA_PERMS_STRICT) ? "strict" : "relaxed");
    g_logged = 1;
}

int vita_permissions_mode(void) {
    if (g_mode < 0) {
        g_mode = vita_permissions_load(NULL);
        if (!g_logged) {
            g_logged = 1;
            perm_log("[PERM] mode=%s (from defaults.cfg; missing = relaxed)\n",
                     (g_mode == VITA_PERMS_STRICT) ? "strict" : "relaxed");
        }
    }
    return g_mode;
}

/* ------------------------------------------------------------------ */
/* The eight symbols generic_policy_load.o used to provide            */
/* ------------------------------------------------------------------ */

int permissions_load_domain_list(void **array) {
    *array = (void *)domainTBL;
    return domainTBLsize;
}

int permissions_load_group_list(void **array) {
    *array = (void *)groupTBL;
    return groupTBLsize;
}

int permissions_load_group_permissions(void **array, char *group_name) {
    int i;
    int ret = 0;

    for (i = 0; i < groupTBLsize; i++) {
        if (strcmp(groupTBL[i], group_name) == 0) {
            *array = (void *)group_membersTBL[i].list;
            ret = group_membersTBL[i].size;
            break;
        }
    }
    return ret;
}

/* Nothing here: Permissions.java falls back to using the group name as
 * the dialog text when getGroupMessages returns null (see the
 * `messages[0] = list[i1]` branch), so the permission screen still reads
 * sensibly without shipping six localised strings per group. */
int permissions_load_group_messages(void **array, char *group_name) {
    (void)array;
    (void)group_name;
    return 0;
}

static int strict_value(char *domain_name, char *group_name, int is_max) {
    int i;

    if (strcmp("manufacturer", domain_name) == 0 ||
        strcmp("maximum", domain_name) == 0 ||
        strcmp("operator", domain_name) == 0) {
        return P_ALLOW;
    }
    if (strcmp("minimum", domain_name) == 0) {
        return P_NEVER;
    }
    for (i = 0; i < groupTBLsize; i++) {
        if (strcmp(groupTBL[i], group_name) == 0) {
            if (strcmp("identified_third_party", domain_name) == 0) {
                return is_max ? identifiedTBL[i].maxval
                              : identifiedTBL[i].defval;
            }
            if (strcmp("unidentified_third_party", domain_name) == 0) {
                return is_max ? unidentifiedTBL[i].maxval
                              : unidentifiedTBL[i].defval;
            }
            break;
        }
    }
    return P_NEVER;
}

static int policy_value(char *domain_name, char *group_name, int is_max) {
    /* v02.90 forensics: record each distinct domain the VM asks about,
     * once. The domain name is the answer to "did the platform treat this
     * jar as signed?" - "identified_third_party" means a valid certificate
     * was found, "unidentified_third_party" that it was not. Without this
     * line a bug report cannot tell whether signing had any effect. */
    if (domain_name != NULL) {
        static const char *seen[8];
        static int seen_n = 0;
        int i, known = 0;

        for (i = 0; i < seen_n; i++) {
            if (strcmp(seen[i], domain_name) == 0) {
                known = 1;
                break;
            }
        }
        if (!known && seen_n < (int)(sizeof(seen) / sizeof(seen[0]))) {
            seen[seen_n++] = domain_name;
            perm_log("[PERM] domain \"%s\" first query: mode=%s\n",
                     domain_name,
                     (vita_permissions_mode() == VITA_PERMS_STRICT)
                         ? "strict" : "relaxed");
        }
    }

    if (vita_permissions_mode() == VITA_PERMS_RELAXED) {
        /* The `minimum` domain exists purely so a test can deny
         * everything; honour it even in relaxed mode so the TCK/self-test
         * path still works. Everything else is allowed. */
        if (domain_name != NULL && strcmp("minimum", domain_name) == 0) {
            return P_NEVER;
        }
        return P_ALLOW;
    }
    return strict_value(domain_name, group_name, is_max);
}

int permissions_get_default_value(char *domain_name, char *group_name) {
    return policy_value(domain_name, group_name, 0);
}

int permissions_get_max_value(char *domain_name, char *group_name) {
    return policy_value(domain_name, group_name, 1);
}

int permissions_dealloc(void *array) {
    (void)array;   /* all arrays are static, exactly like upstream */
    return 0;
}

void permissions_loading_finished(void) {
    /* Report the effective mode once, when the policy tables have been
     * consumed - this is the line to look for when a game still gets a
     * permission prompt: if it says "relaxed" and a prompt appears, the
     * prompt is coming from somewhere else. */
    if (!g_logged) {
        g_logged = 1;
        perm_log("[PERM] policy loaded, mode=%s\n",
                 (vita_permissions_mode() == VITA_PERMS_STRICT)
                     ? "strict" : "relaxed");
    }
}
