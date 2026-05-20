/*
 * net.c - Network layer for Ghost Loader
 *
 * IRX load order: ps2dev9 -> netman -> smap -> ps2ip
 * NetManInit() after all three IOP modules load.
 * NetManSetLinkMode(AUTO) before ps2ipInit().
 * ps2ipInit() with zeroed addrs, then ps2ip_setconfig() with dhcp_enabled=1.
 * Wait for link up, then poll DHCP_STATE_BOUND.
 */

#ifdef NETWORK

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <netman.h>
#include <ps2ip.h>
#include <fileXio_rpc.h>

#include "net.h"

char net_server_host[64] = NET_DEFAULT_SERVER_HOST;
int  net_server_port     = NET_DEFAULT_SERVER_PORT;
char net_debug_msg[96]   = "";

static void net_debug_set(const char *s)
{
    if (!s) {
        net_debug_msg[0] = '\0';
        return;
    }
    strncpy(net_debug_msg, s, (size_t)(sizeof(net_debug_msg) - 1));
    net_debug_msg[sizeof(net_debug_msg) - 1] = '\0';
}

static void net_debug_fmt(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(net_debug_msg, sizeof(net_debug_msg), fmt, ap);
    va_end(ap);
    net_debug_msg[sizeof(net_debug_msg) - 1] = '\0';
}

/* UI and timing helpers provided by main.c */
extern void UiDrawFrame(const char *title, const char *line1, const char *line2,
                        const char *line3, const char *hint, int pulse);
extern void DelayMs(int ms);

/* Network IRX blobs embedded by build.sh via bin2s */
extern unsigned char ps2dev9_irx[];
extern unsigned int  size_ps2dev9_irx;
extern unsigned char netman_irx[];
extern unsigned int  size_netman_irx;
extern unsigned char smap_irx[];
extern unsigned int  size_smap_irx;
extern unsigned char ps2ip_irx[];
extern unsigned int  size_ps2ip_irx;

/* Leaderboard globals */
LbCategory lb_categories[MAX_CATEGORIES];
int        lb_category_count = 0;
LbEntry    lb_entries[MAX_LB_ENTRIES];
int        lb_count = 0;

/* JSON receive buffer — large enough for full index.json with hierarchy */
static char json_buf[32768];

static void trim_end(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

void NetLoadServerConfig(void)
{
    int fd, n;
    char buf[256];
    char *s, *colon, *endp;
    long portv;

    strncpy(net_server_host, NET_DEFAULT_SERVER_HOST, sizeof(net_server_host) - 1);
    net_server_host[sizeof(net_server_host) - 1] = '\0';
    net_server_port = NET_DEFAULT_SERVER_PORT;

    fd = fileXioOpen("mass:ghost_server.txt", O_RDONLY, 0);
    if (fd < 0)
        return;

    n = fileXioRead(fd, buf, (int)sizeof(buf) - 1);
    fileXioClose(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        memmove(buf, buf + 3, (size_t)(strlen(buf) + 1));

    s = buf;
    {
        char *eol = strpbrk(s, "\r\n");
        if (eol)
            *eol = '\0';
    }
    trim_end(s);
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0' || *s == '#')
        return;

    colon = strchr(s, '#');
    if (colon) {
        *colon = '\0';
        trim_end(s);
    }

    colon = strrchr(s, ':');
    if (colon && colon > s && colon[1] >= '0' && colon[1] <= '9') {
        portv = strtol(colon + 1, &endp, 10);
        if (endp && *endp == '\0' && portv >= 1L && portv <= 65535L) {
            *colon = '\0';
            trim_end(s);
            if (*s != '\0') {
                strncpy(net_server_host, s, sizeof(net_server_host) - 1);
                net_server_host[sizeof(net_server_host) - 1] = '\0';
                net_server_port = (int)portv;
            }
            return;
        }
    }

    strncpy(net_server_host, s, sizeof(net_server_host) - 1);
    net_server_host[sizeof(net_server_host) - 1] = '\0';
}

/* =========================================================================
 * JSON helpers
 * ========================================================================= */

/* Extract a string value for key from a flat JSON object fragment. */
static int json_get_str(const char *json, const char *key, char *out, int maxlen)
{
    char search[64];
    const char *p;
    int i = 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && i < maxlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/*
 * Parse time_display like "0:23.26" (M:SS.hh) into total centiseconds for sorting.
 * Returns 1 on success, 0 if invalid.
 */
static int ParseTimeDisplayToCentisec(const char *s, int *out_cs)
{
    char *end = NULL;
    long min, sec;
    int frac = 0;
    int frac_digits = 0;
    const char *p = s;

    if (!s || !*s || !out_cs)
        return 0;

    min = strtol(p, &end, 10);
    if ((const char *)end == p || *end != ':')
        return 0;
    p = (const char *)end + 1;

    sec = strtol(p, &end, 10);
    if ((const char *)end == p)
        return 0;

    if (*end == '.') {
        p = (const char *)end + 1;
        while (*p >= '0' && *p <= '9' && frac_digits < 2) {
            frac = frac * 10 + (int)(*p - '0');
            p++;
            frac_digits++;
        }
        if (frac_digits == 1)
            frac *= 10;
    }

    if (min < 0 || min > 999 || sec < 0 || sec > 59)
        return 0;

    *out_cs = (int)(min * 6000L + sec * 100L + frac);
    return 1;
}

/*
 * Advance p past whitespace/commas to the next '{', extract the balanced
 * brace object into out[], and return a pointer just past the closing '}'.
 * Returns NULL if no object found or output too small.
 * NOTE: does not handle '{' or '}' inside string values — fine for our data.
 */
static const char *json_next_obj(const char *p, char *out, int maxlen)
{
    const char *start;
    int depth = 0, len;

    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;

    start = p;
    do {
        if      (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    } while (*p && depth > 0);

    len = (int)(p - start);
    if (len >= maxlen) len = maxlen - 1;
    strncpy(out, start, len);
    out[len] = '\0';
    return p;
}

/*
 * Advance p past whitespace/commas to the next '"', extract the quoted
 * string into out[], and return a pointer just past the closing '"'.
 * Returns NULL if the next non-whitespace char is ']' (end of array).
 */
static const char *json_next_str_in_array(const char *p, char *out, int maxlen)
{
    int i = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    if (!*p || *p == ']') return NULL;
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"' && i < maxlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

/* =========================================================================
 * parse_leaderboard
 * Parses the full index.json into lb_categories[] and lb_entries[].
 * ========================================================================= */
static int parse_leaderboard(const char *json)
{
    const char *p;
    int cat_count = 0, ghost_count = 0;

    /* ---- Pass 1: categories hierarchy ---- */
    p = strstr(json, "\"categories\"");
    if (p) {
        p = strchr(p, '[');
        if (p) p++;

        while (p && cat_count < MAX_CATEGORIES) {
            /* Each category object can be large (contains all homeworlds) */
            static char cat_obj[16384];
            LbCategory *cat;
            const char *hw_p;
            int hw_count = 0;

            p = json_next_obj(p, cat_obj, sizeof(cat_obj));
            if (!p) break;

            cat = &lb_categories[cat_count];
            memset(cat, 0, sizeof(*cat));
            if (!json_get_str(cat_obj, "name", cat->name, sizeof(cat->name)))
                continue;

            hw_p = strstr(cat_obj, "\"homeworlds\"");
            if (hw_p) {
                hw_p = strchr(hw_p, '[');
                if (hw_p) hw_p++;

                while (hw_p && hw_count < MAX_HOMEWORLDS_PER_CAT) {
                    static char hw_obj[2048];
                    LbHomeworld *hw;
                    const char *lv_p;
                    int lv_count = 0;

                    hw_p = json_next_obj(hw_p, hw_obj, sizeof(hw_obj));
                    if (!hw_p) break;

                    hw = &cat->homeworlds[hw_count];
                    memset(hw, 0, sizeof(*hw));
                    if (!json_get_str(hw_obj, "name", hw->name, sizeof(hw->name)))
                        continue;

                    lv_p = strstr(hw_obj, "\"levels\"");
                    if (lv_p) {
                        lv_p = strchr(lv_p, '[');
                        if (lv_p) lv_p++;
                        while (lv_p && lv_count < MAX_LEVELS_PER_HW) {
                            char lv_name[48];
                            lv_p = json_next_str_in_array(lv_p, lv_name, sizeof(lv_name));
                            if (!lv_p) break;
                            strncpy(hw->levels[lv_count].name, lv_name, 47);
                            hw->levels[lv_count].name[47] = '\0';
                            lv_count++;
                        }
                    }
                    hw->level_count = lv_count;
                    hw_count++;
                }
            }
            cat->homeworld_count = hw_count;
            cat_count++;
        }
    }
    lb_category_count = cat_count;

    /* ---- Pass 2: ghosts flat array ---- */
    p = strstr(json, "\"ghosts\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;

    while (ghost_count < MAX_LB_ENTRIES) {
        char obj[512];
        LbEntry *e;

        p = json_next_obj(p, obj, sizeof(obj));
        if (!p) break;

        e = &lb_entries[ghost_count];
        memset(e, 0, sizeof(*e));

        json_get_str(obj, "category",     e->category,     sizeof(e->category));
        json_get_str(obj, "level_name",   e->level_name,   sizeof(e->level_name));
        json_get_str(obj, "author",       e->author,       sizeof(e->author));
        json_get_str(obj, "time_display", e->time_display, sizeof(e->time_display));
        json_get_str(obj, "filename",     e->filename,     sizeof(e->filename));

        if (e->category[0] && e->level_name[0] &&
            e->time_display[0] && ParseTimeDisplayToCentisec(e->time_display, &e->time_cs))
            ghost_count++;
    }

    lb_count = ghost_count;
    return ghost_count;
}

/* =========================================================================
 * GetGhostsForLevel
 * Returns indices into lb_entries[] for the given category+level,
 * sorted ascending by time (time_cs, fastest first).
 * ========================================================================= */
int GetGhostsForLevel(const char *category, const char *level_name,
                      int *out_indices, int max_out)
{
    int i, count = 0;

    for (i = 0; i < lb_count && count < max_out; i++) {
        if (strcmp(lb_entries[i].category,   category)   == 0 &&
            strcmp(lb_entries[i].level_name, level_name) == 0)
        {
            out_indices[count++] = i;
        }
    }

    /* Insertion sort by time_cs ascending */
    {
        int j, k, tmp;
        for (j = 1; j < count; j++) {
            tmp = out_indices[j];
            k = j - 1;
            while (k >= 0 && lb_entries[out_indices[k]].time_cs >
                              lb_entries[tmp].time_cs) {
                out_indices[k + 1] = out_indices[k];
                k--;
            }
            out_indices[k + 1] = tmp;
        }
    }

    return count;
}

/* =========================================================================
 * NetInit
 * Sequence matches ps2sdk tcpip-dhcp sample:
 *   dev9 -> netman -> smap -> NetManInit -> SetLinkMode -> ps2ip ->
 *   ps2ipInit -> setconfig(dhcp) -> wait link -> wait DHCP bound
 * Returns 0 on success, -1 module load error, -2 DHCP timeout.
 * ========================================================================= */
int NetInit(void)
{
    int ret, modRes, i;
    t_ip_info info;
    struct ip4_addr zero_addr = { 0 };

    UiDrawFrame("Connecting to Ghost Leaderboard", "Loading network modules...",
                "", "", "Please wait.", 0);

    ret = SifExecModuleBuffer(ps2dev9_irx, size_ps2dev9_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;

    ret = SifExecModuleBuffer(netman_irx, size_netman_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;

    ret = SifExecModuleBuffer(smap_irx, size_smap_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;

    NetManInit();
    NetManSetLinkMode(NETMAN_NETIF_ETH_LINK_MODE_AUTO);

    UiDrawFrame("Connecting to Ghost Leaderboard", "Loading TCP/IP stack...",
                "", "", "Please wait.", 4);

    ret = SifExecModuleBuffer(ps2ip_irx, size_ps2ip_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;

    ps2ipInit(&zero_addr, &zero_addr, &zero_addr);

    memset(&info, 0, sizeof(info));
    ps2ip_getconfig("sm0", &info);
    info.dhcp_enabled = 1;
    ps2ip_setconfig(&info);

    /* Wait for physical link (up to 10 s) */
    for (i = 0; i < 50; i++) {
        int link = NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);
        char diag[64];
        snprintf(diag, sizeof(diag), "link_state=%d  iter=%d", link, i);
        UiDrawFrame("Connecting to Ghost Leaderboard", "Waiting for ethernet link...",
                    diag, "Check cable is connected.", "Please wait.", i & 31);
        if (link == NETMAN_NETIF_ETH_LINK_STATE_UP)
            break;
        DelayMs(200);
    }

    /* Poll for DHCP_STATE_BOUND (up to 60 s) */
    for (i = 0; i < 300; i++) {
        char diag1[64], diag2[64];
        u32 ip_val = 0;
        int link;
        memset(&info, 0, sizeof(info));
        ps2ip_getconfig("sm0", &info);
        memcpy(&ip_val, &info.ipaddr, 4);
        link = NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);

        snprintf(diag1, sizeof(diag1), "st=%u en=%u link=%d i=%d",
                 (unsigned)info.dhcp_status, (unsigned)info.dhcp_enabled, link, i);
        snprintf(diag2, sizeof(diag2), "ip=%u.%u.%u.%u",
                 ip_val & 0xFF, (ip_val >> 8) & 0xFF,
                 (ip_val >> 16) & 0xFF, (ip_val >> 24) & 0xFF);

        if (info.dhcp_enabled &&
            (info.dhcp_status == DHCP_STATE_BOUND ||
             info.dhcp_status == DHCP_STATE_OFF)) {
            return 0;
        }
        UiDrawFrame("Connecting to Ghost Leaderboard",
                    "Waiting for network (DHCP)...",
                    diag1, diag2, "Connecting", i & 31);
        DelayMs(200);
    }

    return -2;
}

/* =========================================================================
 * HttpGet (bounded blocking — avoids indefinite hangs on bad IP/DNS/firewall)
 * ========================================================================= */

/* tcpip.h: lwip_ioctl supports FIONBIO; F_SETFL via lwip_fcntl is not reliable. */
#define HTTP_CONNECT_TIMEOUT_SEC 25
/* Do not set SO_RCVTIMEO/SNDTIMEO with int ms here: ps2ip lwIP expects struct timeval
 * unless LWIP_SO_SNDRCVTIMEO_NONSTANDARD; wrong type causes sub-minute bogus timeouts. */

static void http_copy_trim_host(const char *src, char *dst, int dstsz)
{
    int n;
    if (!src || !dst || dstsz < 2) {
        if (dst && dstsz > 0)
            dst[0] = '\0';
        return;
    }
    while (*src == ' ' || *src == '\t')
        src++;
    n = (int)strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
            src[n - 1] == '\r' || src[n - 1] == '\n'))
        n--;
    if (n > dstsz - 1)
        n = dstsz - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* Non-blocking connect + select. Uses FIONBIO (lwip_ioctl); fcntl+O_NONBLOCK is unreliable. */
static int http_set_nonblock(int sock, int on)
{
    unsigned long opt = (unsigned long)(on != 0);
    return lwip_ioctl(sock, FIONBIO, (void *)&opt);
}

/* Non-blocking connect + select so unreachable hosts return instead of stalling. */
static int http_connect_with_timeout(int sock, const struct sockaddr *addr, int addrlen, int timeout_sec)
{
    fd_set wfds;
    struct timeval tv;
    int r, soe;
    socklen_t soelen;

    if (http_set_nonblock(sock, 1) != 0) {
        r = lwip_connect(sock, addr, addrlen);
        if (r < 0)
            net_debug_set("connect: FIONBIO+blocking connect failed");
        return (r < 0) ? -1 : 0;
    }

    r = lwip_connect(sock, addr, addrlen);
    if (r == 0) {
        (void)http_set_nonblock(sock, 0);
        return 0;
    }

    /* Non-blocking: errno may be EINPROGRESS; lwIP values may not match newlib, so do not filter here. */
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;
    r = lwip_select(sock + 1, NULL, &wfds, NULL, &tv);
    if (r == 0) {
        net_debug_fmt("connect: timed out after %ds", timeout_sec);
        (void)http_set_nonblock(sock, 0);
        return -1;
    }
    if (r < 0) {
        net_debug_set("connect: select() failed on socket");
        (void)http_set_nonblock(sock, 0);
        return -1;
    }
    if (!FD_ISSET(sock, &wfds)) {
        net_debug_set("connect: socket not writable after select");
        (void)http_set_nonblock(sock, 0);
        return -1;
    }

    soe    = 0;
    soelen = (socklen_t)sizeof(soe);
    if (lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &soe, &soelen) < 0) {
        net_debug_set("connect: getsockopt(SO_ERROR) failed");
        (void)http_set_nonblock(sock, 0);
        return -1;
    }
    if (soe != 0) {
        net_debug_fmt("connect: SO_ERROR=%d (refused/unreachable?)", soe);
        (void)http_set_nonblock(sock, 0);
        return -1;
    }

    (void)http_set_nonblock(sock, 0);
    return 0;
}

/* host: trimmed, single name or IPv4 (see http_copy_trim_host). */
static int resolve_host_ipv4(const char *host, struct in_addr *out)
{
    struct hostent *he;
    ip4_addr_t ip4;
    u32 a;

    if (!host || !*host || !out)
        return -1;

    /* ip4addr_aton first — never send dotted-quad strings to the DNS path by mistake. */
    if (ip4addr_aton(host, &ip4)) {
        out->s_addr = ip4_addr_get_u32(&ip4);
        return 0;
    }

    a = inet_addr(host);
    if (a != (u32)INADDR_NONE) {
        out->s_addr = a;
        return 0;
    }

#ifdef PS2IP_DNS
    he = lwip_gethostbyname(host);
#else
    he = NULL;
#endif
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0])
        return -1;

    memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
    return 0;
}

int HttpGet(const char *host, int port, const char *path, char *buf, int maxlen)
{
    int sock, ret, total, i;
    struct sockaddr_in addr;
    char req[512];
    char htrim[68];
    int header_end = -1;
    int body_len;

    net_debug_msg[0] = '\0';

    http_copy_trim_host(host, htrim, sizeof(htrim));
    if (!htrim[0]) {
        net_debug_set("GET: host empty after trim (check host:port in ghost_server.txt)");
        return -2;
    }

    sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        net_debug_set("GET: lwip_socket() failed");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if (resolve_host_ipv4(htrim, &addr.sin_addr) < 0) {
        net_debug_fmt("GET: resolve failed for host '%.40s' (not IPv4? DNS?)", htrim);
        lwip_close(sock);
        return -2;
    }

    if (http_connect_with_timeout(sock, (struct sockaddr *)&addr, (int)sizeof(addr),
            HTTP_CONNECT_TIMEOUT_SEC) < 0) {
        lwip_close(sock);
        return -2;
    }

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, htrim);
    if (lwip_send(sock, req, (int)strlen(req), 0) < 0) {
        net_debug_set("GET: send(request) failed");
        lwip_close(sock);
        return -2;
    }

    total = 0;
    while (total < maxlen - 1) {
        ret = lwip_recv(sock, buf + total, (size_t)(maxlen - 1 - total), 0);
        if (ret <= 0)
            break;
        total += ret;
        if (header_end < 0 && total >= 4) {
            for (i = 0; i <= total - 4; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    header_end = i + 4;
                    break;
                }
            }
        }
    }
    lwip_close(sock);

    if (total <= 0) {
        net_debug_set("GET: no bytes received (server closed? wrong port?)");
        return -3;
    }
    if (header_end < 0) {
        net_debug_fmt("GET: no blank line after headers, got %d bytes", total);
        return -3;
    }
    if (strncmp(buf, "HTTP/", 5) != 0) {
        net_debug_set("GET: not an HTTP/1.x response (bad proxy/SSL only?)");
        return -4;
    }
    if (buf[9] != '2') {
        net_debug_fmt("GET: HTTP status not 2xx (status[9]=%c). Check URL/path and server.", buf[9]);
        return -5;
    }

    body_len = total - header_end;
    if (body_len <= 0) {
        net_debug_set("GET: empty body after response headers");
        return -6;
    }

    memmove(buf, buf + header_end, body_len);
    if (body_len < maxlen) buf[body_len] = '\0';

    /* Success: clear any non-fatal noise from the connect path */
    net_debug_msg[0] = '\0';
    return body_len;
}

void LbLevelNameToSlug(const char *display_name, char *out, int out_sz)
{
    int i = 0;

    if (!display_name || !out || out_sz < 2) {
        if (out)
            out[0] = '\0';
        return;
    }

    for (; *display_name && i < out_sz - 1; display_name++) {
        unsigned char c = (unsigned char)*display_name;

        if (c == ' ' || c == '\t') {
            if (i > 0 && out[i - 1] != '_')
                out[i++] = '_';
        }
        else if (c == '\'') {
            /* skip apostrophe: Gnasty's -> gnastys */
        }
        else if (c >= 'A' && c <= 'Z') {
            out[i++] = (char)(c - 'A' + 'a');
        }
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out[i++] = (char)c;
        }
    }
    while (i > 0 && out[i - 1] == '_')
        i--;
    out[i] = '\0';
}

/* =========================================================================
 * Debug stub leaderboard (GHOST_LOADER_DEBUG_UI — no network)
 * ========================================================================= */
void LbLoadDebugLeaderboardStub(void)
{
#ifdef GHOST_LOADER_DEBUG_UI
    static const char kJson[] =
        "{\"version\":2,\"categories\":[{\"name\":\"120%\",\"homeworlds\":[{"
        "\"name\":\"Artisans\",\"levels\":[\"Stone Hill\",\"Dark Hollow\",\"Town Square\"]},"
        "{\"name\":\"Peace Keepers\",\"levels\":[\"Dry Canyon\",\"Cliff Town\"]}"
        "]}],"
        "\"ghosts\":["
        "{\"category\":\"120%\",\"level_name\":\"Stone Hill\",\"author\":\"EmuA\","
        "\"time_display\":\"1:23.26\",\"filename\":\"ghosts/debug_a.bin\"},"
        "{\"category\":\"120%\",\"level_name\":\"Stone Hill\",\"author\":\"EmuB\","
        "\"time_display\":\"2:30.45\",\"filename\":\"ghosts/debug_b.bin\"},"
        "{\"category\":\"120%\",\"level_name\":\"Dry Canyon\",\"author\":\"EmuC\","
        "\"time_display\":\"0:45.00\",\"filename\":\"ghosts/debug_c.bin\"}"
        "]}";

    memset(lb_categories, 0, sizeof(lb_categories));
    memset(lb_entries, 0, sizeof(lb_entries));
    lb_category_count = 0;
    lb_count            = 0;
    parse_leaderboard(kJson);
#else
    (void)0;
#endif
}

/* =========================================================================
 * FetchLeaderboard
 * ========================================================================= */
int FetchLeaderboard(void)
{
    int n;

    UiDrawFrame("Connecting to Ghost Leaderboard", "Downloading leaderboard...",
                "Fetching " NET_INDEX_PATH " from server.",
                net_server_host, "Connecting", 12);

    n = HttpGet(net_server_host, net_server_port, NET_INDEX_PATH,
                json_buf, (int)sizeof(json_buf));
    if (n < 0) return n;

    return parse_leaderboard(json_buf);
}

/* =========================================================================
 * DownloadGhost
 * ========================================================================= */
int DownloadGhost(int idx, unsigned char *ghost_buf, int buf_size)
{
    char path[160];

    if (idx < 0 || idx >= lb_count) return -1;

    snprintf(path, sizeof(path), "/%s", lb_entries[idx].filename);

    return HttpGet(net_server_host, net_server_port, path,
                   (char *)ghost_buf, buf_size);
}

#endif /* NETWORK */
