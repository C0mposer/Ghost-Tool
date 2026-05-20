#ifndef NET_H
#define NET_H

/* ============================================================
 * SERVER CONFIGURATION
 *
 * Defaults apply when mass:ghost_server.txt is missing or unreadable.
 * Put ghost_server.txt on the USB drive root: first line is IPv4, hostname,
 * or host:port (optional UTF-8 BOM, # starts a comment to end of line).
 * Hostnames use lwIP DNS (servers from DHCP); plain IPv4 skips DNS. Requires -DPS2IP_DNS
 * on net.c (build.sh sets this) or hostnames will never resolve.
 * ============================================================ */
/* Hostname only — path is NET_INDEX_PATH; must fit in net_server_host[64]. */
#define NET_DEFAULT_SERVER_HOST "170.9.25.228"
#define NET_DEFAULT_SERVER_PORT 8080
#define NET_INDEX_PATH          "/index.json"

extern char net_server_host[64];
extern int  net_server_port;

/* TEMP: last failure detail for on-screen debug (remove after network is stable). */
extern char net_debug_msg[96];

/* Reload host/port from mass:ghost_server.txt (fileXio must be inited). */
void NetLoadServerConfig(void);

/* ============================================================
 * Hierarchy limits
 * ============================================================ */
#define MAX_CATEGORIES        8
#define MAX_HOMEWORLDS_PER_CAT 12
#define MAX_LEVELS_PER_HW     20
#define MAX_LB_ENTRIES       256

/* One level name inside a homeworld */
typedef struct {
    char name[48];
} LbLevel;

/* One homeworld inside a category */
typedef struct {
    char    name[32];
    LbLevel levels[MAX_LEVELS_PER_HW];
    int     level_count;
} LbHomeworld;

/* One category (120%, Vortex, Any%, …) */
typedef struct {
    char        name[32];
    LbHomeworld homeworlds[MAX_HOMEWORLDS_PER_CAT];
    int         homeworld_count;
} LbCategory;

/* One ghost submission */
typedef struct {
    char category[32];
    char level_name[48];
    char author[32];
    int  time_cs; /* total centiseconds, parsed from time_display for sorting */
    char time_display[16];
    char filename[128];
} LbEntry;

extern LbCategory lb_categories[MAX_CATEGORIES];
extern int        lb_category_count;
extern LbEntry    lb_entries[MAX_LB_ENTRIES];
extern int        lb_count;

/* Load network IOP modules and wait for DHCP.
   Returns 0 on success, -1 module load error, -2 DHCP timeout. */
int NetInit(void);

/* HTTP/1.0 GET — returns body byte count or <0. */
int HttpGet(const char* host, int port, const char* path, char* buf, int maxlen);

/* "Stone Hill" -> stone_hill (matches Skybox-Assets names and ghost-server/skybox .raw files). */
void LbLevelNameToSlug(const char* display_name, char* out, int out_sz);

/* When GHOST_LOADER_DEBUG_UI is defined: fill leaderboard from built-in JSON (no HTTP). */
void LbLoadDebugLeaderboardStub(void);

/* Download and parse index.json. Fills lb_categories and lb_entries.
   Returns total ghost count or <0 on error. */
int FetchLeaderboard(void);

/* Download the ghost .bin for lb_entries[idx] into ghost_buf.
   Returns byte count or <0 on error. */
int DownloadGhost(int idx, unsigned char* ghost_buf, int buf_size);

/* Fill out_indices[] with indices into lb_entries[] matching category+level,
   sorted ascending by time (time_display -> time_cs). Returns count found. */
int GetGhostsForLevel(const char* category, const char* level_name,
    int* out_indices, int max_out);

#endif /* NET_H */
