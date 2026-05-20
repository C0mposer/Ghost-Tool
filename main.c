/*
 * Ghost Tool PS2 homebrew for Saving & Loading ghosts for Spyro 1 Practice Rom.
 *
 * Save: ghostrd.irx runs on the current IOP and copies (0xA29000)
 * into EE RAM; the EE then writes that buffer to USB.
 *
 * Load: ghostwr.irx receives ghost data over RPC and writes to DECKARD RAM from files on USB.
 *
 * For Save, preread via ghostrd must run before any IOP reset, because reset clears DECKARD.
 *
 * Load and Save both prefer the USB/fileXio stack that launched us from uLaunchELF when it is available.
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <iopheap.h>
#include <debug.h>
#include <libpad.h>
#include <fileXio_rpc.h>
#include <dma.h>
#include <packet.h>
#include <graph.h>
#include <gs_psm.h>
#include <draw.h>
#include <draw2d.h>
#include <font.h>
#include <audsrv.h>
#ifdef NETWORK
#include "net.h"
#include "ui_sky_preview.h"
#endif
#include "ui_tex.h"

#define UI_TEX_BACKDROP_SLOT 0

/* IOP modules embedded in ELF via bin2s (see build.sh) */
extern unsigned char ghostwr_irx[];
extern unsigned int  size_ghostwr_irx;

extern unsigned char ghostrd_irx[];
extern unsigned int  size_ghostrd_irx;

extern unsigned char iomanX_irx[];
extern unsigned int  size_iomanX_irx;

extern unsigned char fileXio_irx[];
extern unsigned int  size_fileXio_irx;

extern unsigned char usbd_irx[];
extern unsigned int  size_usbd_irx;

extern unsigned char usbhdfsd_irx[];
extern unsigned int  size_usbhdfsd_irx;

extern unsigned char audsrv_irx[];
extern unsigned int  size_audsrv_irx;

extern unsigned char sfx_wav[];
extern unsigned int  size_sfx_wav;

/* =========================================================================
 * Ghost Data (Matches the Practice ROM exactly)
 * ========================================================================= */

#define GHOST_V2_MAGIC          0x47484432u /* 'GHD2' */
#define GHOST_V2_VERSION        2
#define GHOST_MAX_DRAGON_EVENTS 16
#define GHOST_LEGACY_HEADER_SIZE 24u
#define GHOST_FRAME_SIZE        24u
#define GHOST_MAX_TIME_FRAMES   11000

typedef struct {
    int frameIndex;
    int durationFrames;
} GhostDragonEvent;

typedef struct {
    int initialX;
    int initialY;
    int initialZ;
    int levelId;
    int frameCount;
    int finalTimeFrames;
    int magic;
    int version;
    int headerSize;
    int dragonEventCount;
    GhostDragonEvent dragonEvents[GHOST_MAX_DRAGON_EVENTS];
} GhostHeader; /* V1 prefix: 24 bytes; V2 full header: 168 bytes */

/* =========================================================================
 * IOP Memory / RPC Constants
 * ========================================================================= */

#define GHOST_REGION_SIZE   0x000237F0u  /* ~142 KB - Ghost A, matches practice mod buffer */
#define GHOST_BOOT_WORDS    8u
#define GHOST_BOOT_BYTES    (GHOST_BOOT_WORDS * 4u)

#define GHOST_BOOT_MAGIC            0x464C5931u /* 'FLY1' */
#define GHOST_BOOT_VERSION          1u
#define GHOST_BOOT_FLAG_FLYIN_REQ   0x1u

#define GHOST_RPC_WR_ID     0x80000A2B
#define WR_CHUNK_SIZE       4096

#define GHOST_RPC_RD_ID     0x80000A29
#define RD_RPC_CHUNK_SIZE   16384

/* =========================================================================
 * Ghost data buffer in EE RAM
 * ========================================================================= */

static unsigned char ghostBuffer[GHOST_REGION_SIZE] __attribute__((aligned(64)));

/* =========================================================================
 * RPC client / send buffers for ghostwr module
 * ========================================================================= */

static SifRpcClientData_t ghostRpc __attribute__((aligned(64)));

typedef struct {
    unsigned int  offset;
    unsigned int  size;
    unsigned char data[WR_CHUNK_SIZE];
} GhostWriteReq;

static GhostWriteReq wrSendBuf __attribute__((aligned(64)));
static unsigned char wrRecvBuf[4] __attribute__((aligned(64)));

static SifRpcClientData_t ghostRdRpc __attribute__((aligned(64)));

typedef struct {
    unsigned int offset;
    unsigned int size;
} GhostReadRdReq;

static GhostReadRdReq rdSendBuf __attribute__((aligned(64)));
static unsigned char rdRecvBuf[RD_RPC_CHUNK_SIZE] __attribute__((aligned(64)));

/* =========================================================================
 * File list
 * ========================================================================= */

#define MAX_FILES    64
#define VISIBLE_ROWS  8
#define ROW_HEIGHT   34

static char fileNames[MAX_FILES][256];
static int  fileCount = 0;

/* =========================================================================
 * Audio (UI SFX)
 * ========================================================================= */

static const unsigned char* uiSfxData = NULL;
static int uiSfxSize = 0;
static int uiAudioReady = 0;

static unsigned int ReadLe32(const unsigned char* p)
{
    return (unsigned int)p[0]
        | ((unsigned int)p[1] << 8)
        | ((unsigned int)p[2] << 16)
        | ((unsigned int)p[3] << 24);
}

static unsigned short ReadLe16(const unsigned char* p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned int GhostBootSwapWord(unsigned int w)
{
    return (w << 24)
        | ((w << 8) & 0x00FF0000u)
        | ((w >> 8) & 0x0000FF00u)
        | (w >> 24);
}

/*
 * Pack one logical handshake field for Deckard RAM: same rule as practice ROM
 * deckard_ghost_boot_store_int / GhostHdrStoreI32 — storage word is a 32-bit
 * byte-reverse of the logical value. IOP copies bytes verbatim, so we always
 * emit little-endian bytes of that storage word (independent of EE host endian).
 */
static void GhostBootPackDeckardWordLe(unsigned char* p, unsigned int logical_u32)
{
    unsigned int storage = GhostBootSwapWord(logical_u32);
    p[0] = (unsigned char)(storage & 0xFFu);
    p[1] = (unsigned char)((storage >> 8) & 0xFFu);
    p[2] = (unsigned char)((storage >> 16) & 0xFFu);
    p[3] = (unsigned char)((storage >> 24) & 0xFFu);
}

static int UiAudioInit(void)
{
    int modRes, ret;
    const unsigned char* wav = sfx_wav;
    unsigned int wavSize = size_sfx_wav;
    unsigned int off = 12;
    unsigned int fmtChunkSize = 0;
    const unsigned char* fmtChunk = NULL;
    const unsigned char* dataChunk = NULL;
    unsigned int dataChunkSize = 0;

    ret = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;
    if (audsrv_init() != 0) return -1;
    if (wavSize < 44) return -1;
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return -1;

    while (off + 8 <= wavSize) {
        unsigned int chunkSize = ReadLe32(wav + off + 4);
        const unsigned char* chunkData = wav + off + 8;
        unsigned int nextOff = off + 8 + chunkSize + (chunkSize & 1);
        if (nextOff > wavSize) break;
        if (memcmp(wav + off, "fmt ", 4) == 0) { fmtChunk = chunkData; fmtChunkSize = chunkSize; }
        else if (memcmp(wav + off, "data", 4) == 0) { dataChunk = chunkData; dataChunkSize = chunkSize; }
        off = nextOff;
    }

    if (!fmtChunk || !dataChunk || fmtChunkSize < 16 || dataChunkSize == 0) return -1;

    {
        unsigned short audioFormat = ReadLe16(fmtChunk + 0);
        unsigned short channels = ReadLe16(fmtChunk + 2);
        unsigned int   sampleRate = ReadLe32(fmtChunk + 4);
        unsigned short bitsPerSample = ReadLe16(fmtChunk + 14);
        audsrv_fmt_t fmt;

        if (audioFormat != 1) return -1;
        if (!(channels == 1 || channels == 2)) return -1;
        if (!(bitsPerSample == 8 || bitsPerSample == 16)) return -1;

        fmt.freq = (int)sampleRate;
        fmt.bits = (int)bitsPerSample;
        fmt.channels = (int)channels;
        if (audsrv_set_format(&fmt) != 0) return -1;
    }

    audsrv_set_volume(24);
    uiSfxData = dataChunk;
    uiSfxSize = (int)dataChunkSize;
    uiAudioReady = 1;
    return 0;
}

static void UiPlayDing(void)
{
    if (!uiAudioReady || !uiSfxData || uiSfxSize <= 0) return;
    audsrv_stop_audio();
    if (audsrv_wait_audio(uiSfxSize) == 0)
        audsrv_play_audio((const char*)uiSfxData, uiSfxSize);
}

/* =========================================================================
 * Controller
 * ========================================================================= */

static char padBuf[256] __attribute__((aligned(64)));
static int uiPadReady = 0;

#define PAD_PORT 0
#define PAD_SLOT 0

void DelayMs(int ms);
void UiDrawFrame(const char* title, const char* line1, const char* line2, const char* line3, const char* hint, int pulse);

static int WaitPadReadyTimeout(int tries)
{
    int i;
    for (i = 0; i < tries; i++) {
        int state = padGetState(PAD_PORT, PAD_SLOT);
        if (state == PAD_STATE_STABLE)
            return 0;
        DelayMs(20);
    }
    return -1;
}

static unsigned short ReadPadButtons(void)
{
    struct padButtonStatus buttons;
    int state = padGetState(PAD_PORT, PAD_SLOT);
    if (state != PAD_STATE_STABLE) return 0;
    if (padRead(PAD_PORT, PAD_SLOT, &buttons) == 0) return 0;
    return 0xFFFF ^ buttons.btns;
}

static unsigned short WaitForEither(unsigned short btn1, unsigned short btn2)
{
    while (ReadPadButtons() & (btn1 | btn2));
    for (;;) {
        unsigned short btns = ReadPadButtons();
        if (btns & btn1) return btn1;
        if (btns & btn2) return btn2;
    }
}

#ifdef NETWORK
/* After nested UI returns, held buttons must not count as a new edge on the next poll. */
static void UiSyncPadPrev(unsigned short* pPrev)
{
    if (pPrev)
        *pPrev = ReadPadButtons();
}
#endif

/* =========================================================================
 * Display Helpers
 * ========================================================================= */

static const char* GetLevelName(int levelId)
{
    switch (levelId) {
        case 10: return "Artisans Home";
        case 11: return "Stone Hill";
        case 12: return "Dark Hollow";
        case 13: return "Town Square";
        case 14: return "Toasty";
        case 15: return "Sunny Flight";
        case 20: return "Peace Keepers Home";
        case 21: return "Dry Canyon";
        case 22: return "Cliff Town";
        case 23: return "Ice Cavern";
        case 24: return "Doctor Shemp";
        case 25: return "Night Flight";
        case 30: return "Magic Crafters Home";
        case 31: return "Alpine Ridge";
        case 32: return "High Caves";
        case 33: return "Wizard Peak";
        case 34: return "Blowhard";
        case 35: return "Crystal Flight";
        case 40: return "Beast Makers Home";
        case 41: return "Terrace Village";
        case 42: return "Misty Bog";
        case 43: return "Tree Tops";
        case 44: return "Metalhead";
        case 45: return "Wild Flight";
        case 50: return "Dream Weavers Home";
        case 51: return "Dark Passage";
        case 52: return "Lofty Castle";
        case 53: return "Haunted Towers";
        case 54: return "Jacques";
        case 55: return "Icy Flight";
        case 60: return "Gnastys World";
        case 61: return "Gnorc Cove";
        case 62: return "Twilight Harbor";
        case 63: return "Gnasty Gnorc";
        case 64: return "Gnastys Loot";
        default: return "Unknown";
    }
}

static char ghostFilePath[128];

static int FileExists(const char* path)
{
    int fd = fileXioOpen(path, O_RDONLY, 0);
    if (fd >= 0) {
        fileXioClose(fd);
        return 1;
    }
    return 0;
}

static void BuildGhostFilename(int levelId)
{
    const char* name = GetLevelName(levelId);
    char slug[64];
    int si = 0;
    int i;

    for (i = 0; name[i] && si < 58; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
            slug[si++] = c + ('a' - 'A');
        else if (c >= 'a' && c <= 'z')
            slug[si++] = c;
        else if (c >= '0' && c <= '9')
            slug[si++] = c;
        else if (c == ' ' && si > 0 && slug[si - 1] != '_')
            slug[si++] = '_';
    }
    slug[si] = '\0';

    sprintf(ghostFilePath, "mass:ghost_%s.bin", slug);
    if (!FileExists(ghostFilePath))
        return;

    for (i = 2; i <= 99; i++) {
        sprintf(ghostFilePath, "mass:ghost_%s_%d.bin", slug, i);
        if (!FileExists(ghostFilePath))
            return;
    }
    sprintf(ghostFilePath, "mass:ghost_%s.bin", slug);
}

static int SaveToUSB(const void* data, int size, const char* path)
{
    int fd = fileXioOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int written;

    if (fd < 0)
        return -1;

    written = fileXioWrite(fd, data, size);
    fileXioClose(fd);

    if (written != size)
        return -2;

    return 0;
}

void DelayMs(int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        volatile int j;
        for (j = 0; j < 150000; j++);
    }
}

static int InitIopClients(void)
{
    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
    sbv_patch_enable_lmb();
    return 0;
}

static int InitPadRuntime(void)
{
    int ret;

    ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    (void)ret;
    ret = SifLoadModule("rom0:PADMAN", 0, NULL);
    (void)ret;

    if (padInit(0) < 0)
        return -1;
    if (padPortOpen(PAD_PORT, PAD_SLOT, padBuf) == 0)
        return -1;
    if (WaitPadReadyTimeout(250) < 0)
        return -1;
    uiPadReady = 1;
    return 0;
}

static int ProbeUsbMass(void)
{
    int dd = fileXioDopen("mass:");
    if (dd >= 0) {
        fileXioDclose(dd);
        return 1;
    }
    dd = fileXioDopen("mass:/");
    if (dd >= 0) {
        fileXioDclose(dd);
        return 1;
    }
    return 0;
}

static int LoadUsbStackIfNeeded(int* usbLoaded)
{
    int ret, modRes;

    if (*usbLoaded)
        return 0;

    fileXioInit();
    if (ProbeUsbMass()) {
        *usbLoaded = 1;
        return 0;
    }

    ret = SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;
    ret = SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;
    ret = SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;
    ret = SifExecModuleBuffer(usbhdfsd_irx, size_usbhdfsd_irx, 0, NULL, &modRes);
    if (ret < 0) return -1;

    fileXioInit();
    *usbLoaded = 1;
    return 0;
}

static int WaitForUsbMass(const char* title)
{
    unsigned int i;
    unsigned short prevBtns = ReadPadButtons();

    DelayMs(500);
    for (i = 0; i < 50; i++) {
        unsigned short curBtns = ReadPadButtons();
        unsigned short edgeBtns = curBtns & (unsigned short)(~prevBtns);
        prevBtns = curBtns;
        if (edgeBtns & PAD_CIRCLE) {
            UiPlayDing();
            return -1;
        }
        if (ProbeUsbMass())
            return 1;
        UiDrawFrame(title, "Waiting for USB drive...",
            "Insert FAT32 USB storage into console.", "Detecting mass: mount point.",
            "USB scan in progress (O = Back)", (int)(i & 31));
        DelayMs(200);
    }
    return 0;
}

static int ReadGhostDataViaRpcRd(void)
{
    unsigned int offset = 0;

    while (offset < GHOST_REGION_SIZE) {
        unsigned int chunk = GHOST_REGION_SIZE - offset;
        if (chunk > RD_RPC_CHUNK_SIZE)
            chunk = RD_RPC_CHUNK_SIZE;

        rdSendBuf.offset = offset;
        rdSendBuf.size = chunk;

        if (SifCallRpc(&ghostRdRpc, 0, 0,
            &rdSendBuf, sizeof(rdSendBuf),
            rdRecvBuf, chunk,
            NULL, NULL) < 0)
            return -1;

        memcpy(ghostBuffer + offset, rdRecvBuf, chunk);
        offset += chunk;
    }

    return 0;
}

/* Load ghostrd on current IOP and copy DECKARD into ghostBuffer. Must run before any IOP reset. */
static int SaverPrereadDeckard(void)
{
    int ret, modRes;
    int bindRetries;

    ret = SifExecModuleBuffer(ghostrd_irx, size_ghostrd_irx, 0, NULL, &modRes);
    if (ret < 0)
        return -1;

    DelayMs(200);

    memset(&ghostRdRpc, 0, sizeof(ghostRdRpc));
    ret = -1;
    for (bindRetries = 0; bindRetries < 50; bindRetries++) {
        if (SifBindRpc(&ghostRdRpc, GHOST_RPC_RD_ID, 0) >= 0 && ghostRdRpc.server != NULL) {
            ret = 0;
            break;
        }
        DelayMs(50);
    }
    if (ret < 0)
        return -1;

    memset(ghostBuffer, 0, GHOST_REGION_SIZE);
    return ReadGhostDataViaRpcRd();
}

/* =========================================================================
 * UI Rendering
 * ========================================================================= */

static framebuffer_t uiFrame;
static zbuffer_t     uiZ;
static fontx_t       uiFont;
/* Same KROM glyphs; w_margin 0 tightens advance (~20% shorter line vs uiFont) for long hints. */
static fontx_t       uiFontHint;
static int           uiReady = 0;

static void UiSetColor(color_t* c, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    c->r = r; c->g = g; c->b = b; c->a = a; c->q = 1.0f;
}

static void UiSetRect(rect_t* r, float x0, float y0, float x1, float y1, color_t* c)
{
    r->v0.x = x0; r->v0.y = y0; r->v0.z = 2;
    r->v1.x = x1; r->v1.y = y1; r->v1.z = 2;
    r->color = *c;
}

#define UI_FONT_CHAR_W 9
/* Leaderboard time column: keep text clear of row focus ring on the right. */
#define LB_TIME_COL_RIGHT 568.0f

static float UiTextWidthPx(const char* s)
{
    return (float)strlen(s) * (float)UI_FONT_CHAR_W;
}

/* 2 px outline on the selection rectangle (same bounds as the row/tab). */
static qword_t* UiDrawFocusRing(qword_t* q, int ctx, float x0, float y0, float x1, float y1, int pulse)
{
    rect_t    r;
    color_t   c;
    unsigned char a = (unsigned char)(0xA0 + (pulse & 0x0F));
    UiSetColor(&c, 230, 248, 255, a);
    if (x1 <= x0 + 4.0f || y1 <= y0 + 4.0f)
        return q;
    UiSetRect(&r, x0, y0, x1, y0 + 2.0f, &c);
    q = draw_rect_filled(q, ctx, &r);
    UiSetRect(&r, x0, y1 - 2.0f, x1, y1, &c);
    q = draw_rect_filled(q, ctx, &r);
    UiSetRect(&r, x0, y0, x0 + 2.0f, y1, &c);
    q = draw_rect_filled(q, ctx, &r);
    UiSetRect(&r, x1 - 2.0f, y0, x1, y1, &c);
    q = draw_rect_filled(q, ctx, &r);
    return q;
}

/* Darken top or bottom of a clipped list so more content reads as "off-screen". */
static qword_t* UiDrawClipEdgeFade(qword_t* q, int ctx, float x0, float yEdge, float x1, int fromTop, int pulse)
{
    int k;
    for (k = 0; k < 4; k++) {
        color_t c;
        unsigned char a = (unsigned char)(0x14 + (unsigned char)(k * 18) + (unsigned char)((pulse + k * 3) & 7));
        UiSetColor(&c, 4, 10, 20, a);
        if (fromTop) {
            rect_t r;
            float y0 = yEdge + (float)(k * 4);
            UiSetRect(&r, x0, y0, x1, y0 + 4.0f, &c);
            q = draw_rect_filled(q, ctx, &r);
        }
        else {
            rect_t r;
            float y1 = yEdge - (float)(k * 4);
            UiSetRect(&r, x0, y1 - 4.0f, x1, y1, &c);
            q = draw_rect_filled(q, ctx, &r);
        }
    }
    return q;
}

void UiDrawFrameStyled(const char* title, const char* line1, const char* line2, const char* line3, const char* hint, int pulse, int errorStyle)
{
    packet_t* packet;
    qword_t* q;
    rect_t    rect;
    color_t   c;
    vertex_t  v;

    if (!uiReady) return;

    packet = packet_init(8192, PACKET_NORMAL);
    q = packet->data;

    q = draw_clear(q, 0, 0.0f, 0.0f, 640.0f, 448.0f, 8, 12, 18);
    q = UiTexAfterClear(q, 0, UI_TEX_BACKDROP_SLOT);

    UiSetColor(&c, 18, 26, 36, 0x80);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 412.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 34, 53, 72, 0x40);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 82.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    if (errorStyle)
        UiSetColor(&c, 220, 72, 72, (unsigned char)(0x28 + (pulse & 0x1F)));
    else
        UiSetColor(&c, 80, 185, 235, (unsigned char)(0x20 + (pulse & 0x1F)));
    UiSetRect(&rect, 34.0f, 82.0f, 606.0f, 84.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 214, 224, 233, 0x80);
    v.x = 52.0f; v.y = 58.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)title, LEFT_ALIGN, &v, &c, &uiFont);

    if (errorStyle)
        UiSetColor(&c, 236, 132, 132, 0x80);
    else
        UiSetColor(&c, 176, 190, 205, 0x80);
    v.x = 52.0f; v.y = 120.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)line1, LEFT_ALIGN, &v, &c, &uiFont);
    UiSetColor(&c, 176, 190, 205, 0x80);
    v.y = 154.0f;
    q = fontx_print_ascii(q, 0, (const unsigned char*)line2, LEFT_ALIGN, &v, &c, &uiFont);
    v.y = 188.0f;
    q = fontx_print_ascii(q, 0, (const unsigned char*)line3, LEFT_ALIGN, &v, &c, &uiFont);

    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 52.0f; v.y = 366.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)hint, LEFT_ALIGN, &v, &c, &uiFont);

    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    packet_free(packet);
}

void UiDrawFrame(const char* title, const char* line1, const char* line2, const char* line3, const char* hint, int pulse)
{
    UiDrawFrameStyled(title, line1, line2, line3, hint, pulse, 0);
}

static void UiDrawErrorFrame(const char* title, const char* line1, const char* line2, const char* line3, const char* hint, int pulse)
{
    UiDrawFrameStyled(title, line1, line2, line3, hint, pulse, 1);
}

/* Scrollable file-picker screen */
static void UiDrawFileList(int cursor, int scrollOff, int pulse)
{
    packet_t* packet;
    qword_t* q;
    rect_t    rect;
    color_t   c;
    vertex_t  v;
    int i;
    int visEnd = scrollOff + VISIBLE_ROWS;
    if (visEnd > fileCount) visEnd = fileCount;

    if (!uiReady) return;

    packet = packet_init(8192, PACKET_NORMAL);
    q = packet->data;

    q = draw_clear(q, 0, 0.0f, 0.0f, 640.0f, 448.0f, 8, 12, 18);
    q = UiTexAfterClear(q, 0, UI_TEX_BACKDROP_SLOT);

    /* Panel */
    UiSetColor(&c, 18, 26, 36, 0x80);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 412.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    /* Header bar */
    UiSetColor(&c, 34, 53, 72, 0x40);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 82.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    /* Accent line */
    UiSetColor(&c, 80, 185, 235, (unsigned char)(0x20 + (pulse & 0x1F)));
    UiSetRect(&rect, 34.0f, 82.0f, 606.0f, 84.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    /* Title */
    UiSetColor(&c, 214, 224, 233, 0x80);
    v.x = 52.0f; v.y = 58.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)"GHOST LOADER - Select File", LEFT_ALIGN, &v, &c, &uiFont);

    /* Item counter (top-right of header, right-aligned) */
    {
        char countStr[16];
        sprintf(countStr, "%d / %d", cursor + 1, fileCount);
        UiSetColor(&c, 122, 150, 176, 0x80);
        v.x = 598.0f - UiTextWidthPx(countStr); v.y = 58.0f; v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)countStr, LEFT_ALIGN, &v, &c, &uiFont);
    }

    {
        float listTop = 90.0f;
        float listBottom = listTop + (float)(VISIBLE_ROWS * ROW_HEIGHT);
        if (scrollOff > 0)
            q = UiDrawClipEdgeFade(q, 0, 36.0f, listTop, 604.0f, 1, pulse);
        if (visEnd < fileCount)
            q = UiDrawClipEdgeFade(q, 0, 36.0f, listBottom, 604.0f, 0, pulse);
    }

    /* File rows */
    for (i = scrollOff; i < visEnd; i++) {
        float rowY = (float)(90 + (i - scrollOff) * ROW_HEIGHT);

        if (i == cursor) {
            /* Highlight background */
            UiSetColor(&c, 80, 140, 200, 0x30);
            UiSetRect(&rect, 36.0f, rowY, 604.0f, rowY + ROW_HEIGHT - 2, &c);
            q = draw_rect_filled(q, 0, &rect);

            /* Left accent bar */
            UiSetColor(&c, 80, 185, 235, (unsigned char)(0x28 + (pulse & 0x1F)));
            UiSetRect(&rect, 36.0f, rowY, 40.0f, rowY + ROW_HEIGHT - 2, &c);
            q = draw_rect_filled(q, 0, &rect);

            q = UiDrawFocusRing(q, 0, 36.0f, rowY, 604.0f, rowY + ROW_HEIGHT - 2, pulse);
            UiSetColor(&c, 200, 220, 240, 0x80);
        }
        else {
            UiSetColor(&c, 176, 190, 205, 0x60);
        }

        /* Truncate long names to fit the panel */
        {
            char display[42];
            int  len = strlen(fileNames[i]);
            if (len > 40) {
                strncpy(display, fileNames[i], 37);
                display[37] = '.'; display[38] = '.'; display[39] = '.'; display[40] = '\0';
            }
            else {
                strncpy(display, fileNames[i], 40);
                display[40] = '\0';
            }
            v.x = 50.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)display, LEFT_ALIGN, &v, &c, &uiFont);
        }
    }

    /* Scroll arrows */
    UiSetColor(&c, 122, 150, 176, (unsigned char)(0x70 + (pulse & 0x1F)));
    if (scrollOff > 0) {
        v.x = 578.0f; v.y = 94.0f; v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)"^", LEFT_ALIGN, &v, &c, &uiFont);
    }
    if (visEnd < fileCount) {
        v.x = 578.0f; v.y = (float)(90 + (VISIBLE_ROWS - 1) * ROW_HEIGHT + 8); v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)"v", LEFT_ALIGN, &v, &c, &uiFont);
    }

    /* Hint bar */
    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 52.0f; v.y = 366.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)"Up/Down = scroll   X = Load   O = Cancel", LEFT_ALIGN, &v, &c, &uiFont);

    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    packet_free(packet);
}

/* First screen: 0 = Save Ghost, 1 = Load Ghost */
static void UiDrawRootSelect(int cursor, int pulse)
{
    packet_t* packet;
    qword_t* q;
    rect_t    rect;
    color_t   c;
    vertex_t  v;

    if (!uiReady) return;

    packet = packet_init(8192, PACKET_NORMAL);
    q = packet->data;

    q = draw_clear(q, 0, 0.0f, 0.0f, 640.0f, 448.0f, 8, 12, 18);
    q = UiTexAfterClear(q, 0, UI_TEX_BACKDROP_SLOT);

    UiSetColor(&c, 18, 26, 36, 0x80);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 412.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 34, 53, 72, 0x40);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 82.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 80, 185, 235, (unsigned char)(0x20 + (pulse & 0x1F)));
    UiSetRect(&rect, 34.0f, 82.0f, 606.0f, 84.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 214, 224, 233, 0x80);
    v.x = 52.0f; v.y = 58.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)"GHOST TOOL", LEFT_ALIGN, &v, &c, &uiFont);

    {
        const char* labels[2] = {
            "SAVE GHOST    Copy replay from RAM to USB",
#ifdef NETWORK
            "LOAD GHOST    Copy replay from USB or network into RAM"
#else
            "LOAD GHOST    Copy replay from USB into RAM"
#endif
        };
        int i;
        for (i = 0; i < 2; i++) {
            float rowY = (float)(110 + i * 60);
            if (i == cursor) {
                UiSetColor(&c, 80, 140, 200, 0x30);
                UiSetRect(&rect, 36.0f, rowY - 4, 604.0f, rowY + 38, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 80, 185, 235, (unsigned char)(0x28 + (pulse & 0x1F)));
                UiSetRect(&rect, 36.0f, rowY - 4, 40.0f, rowY + 38, &c);
                q = draw_rect_filled(q, 0, &rect);
                q = UiDrawFocusRing(q, 0, 36.0f, rowY - 4, 604.0f, rowY + 38, pulse);
                UiSetColor(&c, 200, 220, 240, 0x80);
            }
            else {
                UiSetColor(&c, 140, 160, 180, 0x60);
            }
            v.x = 52.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)labels[i], LEFT_ALIGN, &v, &c, &uiFont);
        }
    }

    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 52.0f; v.y = 366.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)"Up/Down = select   X = Confirm   O = Back", LEFT_ALIGN, &v, &c, &uiFont);

    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    packet_free(packet);
}

static int UiSelectSaveOrLoad(void)
{
    int cursor = 0, pulse = 0;
    unsigned short prev = ReadPadButtons();
    for (;;) {
        unsigned short cur = ReadPadButtons();
        unsigned short edge = cur & ~prev;
        prev = cur;
        if (edge & PAD_UP)   cursor = 0;
        if (edge & PAD_DOWN) cursor = 1;
        if (edge & PAD_CROSS) return cursor;
        UiDrawRootSelect(cursor, pulse++);
    }
}

/* Mode selector screen: 0 = USB, 1 = Network (disabled unless NETWORK is defined) */
static void UiDrawModeSelect(int cursor, int pulse)
{
    packet_t* packet;
    qword_t* q;
    rect_t    rect;
    color_t   c;
    vertex_t  v;

    if (!uiReady) return;

    packet = packet_init(8192, PACKET_NORMAL);
    q = packet->data;

    q = draw_clear(q, 0, 0.0f, 0.0f, 640.0f, 448.0f, 8, 12, 18);
    q = UiTexAfterClear(q, 0, UI_TEX_BACKDROP_SLOT);

    UiSetColor(&c, 18, 26, 36, 0x80);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 412.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 34, 53, 72, 0x40);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 82.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 80, 185, 235, (unsigned char)(0x20 + (pulse & 0x1F)));
    UiSetRect(&rect, 34.0f, 82.0f, 606.0f, 84.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 214, 224, 233, 0x80);
    v.x = 52.0f; v.y = 58.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)"GHOST LOADER", LEFT_ALIGN, &v, &c, &uiFont);

    /* Option rows */
    {
        const char* labels[2] = {
            "USB       Load ghost file from USB drive",
#ifdef NETWORK
            "NETWORK   Download ghost from online leaderboard"
#else
            "NETWORK   COMING SOON"
#endif
        };
        int i;
        for (i = 0; i < 2; i++) {
            float rowY = (float)(110 + i * 60);
            int disabled = 0;
            #ifndef NETWORK
            disabled = (i == 1);
            #endif
            if (i == cursor && !disabled) {
                UiSetColor(&c, 80, 140, 200, 0x30);
                UiSetRect(&rect, 36.0f, rowY - 4, 604.0f, rowY + 38, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 80, 185, 235, (unsigned char)(0x28 + (pulse & 0x1F)));
                UiSetRect(&rect, 36.0f, rowY - 4, 40.0f, rowY + 38, &c);
                q = draw_rect_filled(q, 0, &rect);
                q = UiDrawFocusRing(q, 0, 36.0f, rowY - 4, 604.0f, rowY + 38, pulse);
                UiSetColor(&c, 200, 220, 240, 0x80);
            }
            else if (disabled) {
                UiSetColor(&c, 35, 42, 50, 0x52);
                UiSetRect(&rect, 36.0f, rowY - 4, 604.0f, rowY + 38, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 90, 104, 116, 0x48);
            }
            else {
                UiSetColor(&c, 140, 160, 180, 0x60);
            }
            v.x = 52.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)labels[i], LEFT_ALIGN, &v, &c, &uiFont);
            #ifndef NETWORK
            if (disabled) {
                UiSetColor(&c, 70, 82, 94, 0x58);
                UiSetRect(&rect, 36.0f, rowY + 16.0f, 604.0f, rowY + 20.0f, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 110, 128, 144, 0x58);
                v.x = 472.0f; v.y = rowY + 8.0f; v.z = 4;
                q = fontx_print_ascii(q, 0, (const unsigned char*)"SOON", LEFT_ALIGN, &v, &c, &uiFont);
            }
            #endif
        }
    }

    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 52.0f; v.y = 366.0f; v.z = 4;
    #ifdef NETWORK
    q = fontx_print_ascii(q, 0, (const unsigned char*)"Up/Down = select   X = Confirm", LEFT_ALIGN, &v, &c, &uiFont);
    #else
    q = fontx_print_ascii(q, 0, (const unsigned char*)"X = Confirm   O = Back", LEFT_ALIGN, &v, &c, &uiFont);
    #endif

    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    packet_free(packet);
}

static int UiSelectMode(void)
{
    int cursor = 0, pulse = 0;
    /* Baseline current held buttons so backing out with O held does not fake edges. */
    unsigned short prev = ReadPadButtons();
    for (;;) {
        unsigned short cur = ReadPadButtons();
        unsigned short edge = cur & ~prev;
        prev = cur;
        #ifdef NETWORK
        if (edge & PAD_UP)    cursor = 0;
        if (edge & PAD_DOWN)  cursor = 1;
        if (edge & PAD_CROSS) return cursor;
        #else
        cursor = 0;
        if (edge & PAD_CROSS) return 0;
        #endif
        if (edge & PAD_CIRCLE) return -1;
        UiDrawModeSelect(cursor, pulse++);
    }
}

#ifdef NETWORK
/* =========================================================================
 * Network leaderboard browser
 *
 * Returns: lb_entries index of the ghost to download, or -1 to go back
 *          to mode select.
 * ========================================================================= */

#define LB_VISIBLE_ROWS  9
#define LB_ROW_H        30
#define LB_STICKY_H     18

#define LB_HW_CHAR_W    9
#define LB_HW_HPAD      24
#define LB_HW_GAP       8
#define LB_HW_ROW_H     26
#define LB_HW_TAB_X0    36.0f
#define LB_HW_MAX_X     602.0f

/* Level browser: left column levels, right column times preview + skybox (unchanged position). */
#define LB_PREVIEW_SPLIT    418.0f /* narrower right pane → more room for level names */
#define LB_LIST_INNER_R     (LB_PREVIEW_SPLIT - 8.0f)
#define LB_SKY_PREVIEW_TOP  248.0f
#define LB_PREVIEW_END_Y    (LB_SKY_PREVIEW_TOP - 8.0f)
#define LB_PREVIEW_ROW_H    19 /* font ~15px tall; extra gap avoids row overlap */
#define LB_PREVIEW_MAX_ROWS 3
#define LB_LEVEL_LIST_BOTTOM 382.0f

#define TROPHY_WR  "[WR] "
#define TROPHY_2   "[2nd]"
#define TROPHY_3   "[3rd]"
#define TROPHY_N   "     "

/* Homeworld tab strip: wrap to next row when wider than panel; returns level list Y and row count.
 * outHwTabBottom (optional): max Y of homeworld tab row bottoms — for sticky section header placement. */
static void LbHomeworldTabLayout(int catIdx, float* outLevelListTop, int* outVisRows,
    float* tabX, float* tabY, float* tabW, int* outCount, float* outHwTabBottom)
{
    int i;
    float x = LB_HW_TAB_X0;
    float y = 82.0f;
    float bottom = y + (float)LB_HW_ROW_H;
    LbCategory* cat;

    if (outCount) *outCount = 0;
    if (outHwTabBottom) *outHwTabBottom = bottom;

    if (catIdx < 0 || catIdx >= lb_category_count || outLevelListTop == NULL || outVisRows == NULL) {
        if (outLevelListTop) *outLevelListTop = 100.0f;
        if (outVisRows) *outVisRows = LB_VISIBLE_ROWS;
        return;
    }

    cat = &lb_categories[catIdx];
    for (i = 0; i < cat->homeworld_count; i++) {
        const char* hwName = cat->homeworlds[i].name;
        float w = (float)((int)strlen(hwName) * LB_HW_CHAR_W + LB_HW_HPAD);

        if (x > LB_HW_TAB_X0 && x + w > LB_HW_MAX_X) {
            x = LB_HW_TAB_X0;
            y += (float)LB_HW_ROW_H;
        }

        if (tabX) tabX[i] = x;
        if (tabY) tabY[i] = y;
        if (tabW) tabW[i] = w;

        x += w + (float)LB_HW_GAP;
        if (y + (float)LB_HW_ROW_H > bottom)
            bottom = y + (float)LB_HW_ROW_H;
    }

    if (outCount) *outCount = cat->homeworld_count;
    if (outHwTabBottom) *outHwTabBottom = bottom;

    {
        float listTop = bottom + 6.0f + (float)LB_STICKY_H + 4.0f;
        int vr = (int)((LB_LEVEL_LIST_BOTTOM - listTop) / (float)LB_ROW_H);
        if (vr < 1) vr = 1;
        if (vr > LB_VISIBLE_ROWS) vr = LB_VISIBLE_ROWS;
        *outLevelListTop = listTop;
        *outVisRows = vr;
    }
}

/* Right column: compact top times (max 8, height-limited) + skybox below (drawn after this). */
static qword_t* UiDrawLevelPreviewPanel(qword_t* q, float yPanelTop,
    const char* catName, const char* levelName, int pulse, int previewSlide)
{
    rect_t    rect;
    color_t   c;
    vertex_t  v;
    int       indices[MAX_LB_ENTRIES];
    int       count;
    int       i;
    int       maxRows;
    int       show;
    float     panelX0 = LB_PREVIEW_SPLIT;
    float     panelX1 = 604.0f;
    /* Slide is capped so times/title never extend past 640 (previewSlide*10 hit ~644, which can
     * destabilize font/GS and show as subtle one-line or edge glitches). */
    float     slideX = (float)(previewSlide * 8);
    if (slideX > 22.0f) slideX = 22.0f;
    float     innerTop;
    char      titleBuf[44];

    if (!catName || !levelName)
        return q;

    strncpy(titleBuf, levelName, sizeof(titleBuf) - 1);
    titleBuf[sizeof(titleBuf) - 1] = '\0';
    if (strlen(titleBuf) > 22) {
        titleBuf[19] = '.';
        titleBuf[20] = '.';
        titleBuf[21] = '.';
        titleBuf[22] = '\0';
    }

    UiSetColor(&c, 16, 26, 38, 0x82);
    UiSetRect(&rect, panelX0, yPanelTop, panelX1, 384.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 34, 55, 78, 0x55);
    UiSetRect(&rect, panelX0 + 2.0f, yPanelTop + 2.0f, panelX1 - 2.0f, yPanelTop + 16.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 210, 225, 245, 0x92);
    v.x = panelX0 + 8.0f + slideX;
    v.y = yPanelTop + 4.0f;
    v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)titleBuf, LEFT_ALIGN, &v, &c, &uiFont);

    innerTop = yPanelTop + 19.0f;
    maxRows = (int)((LB_PREVIEW_END_Y - innerTop) / (float)LB_PREVIEW_ROW_H);
    if (maxRows < 1) maxRows = 1;
    if (maxRows > LB_PREVIEW_MAX_ROWS) maxRows = LB_PREVIEW_MAX_ROWS;

    count = GetGhostsForLevel(catName, levelName, indices, MAX_LB_ENTRIES);
    if (count <= 0) {
        UiSetColor(&c, 130, 155, 180, 0x62);
        v.x = panelX0 + 8.0f + slideX;
        v.y = innerTop + 2.0f;
        v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)"No ghosts yet", LEFT_ALIGN, &v, &c, &uiFont);
        return q;
    }

    show = count;
    if (show > maxRows) show = maxRows;

    /* Preview: fixed-width rank + trophy (left), time (right). Names stay on full X view. */
    for (i = 0; i < show; i++) {
        LbEntry* e = &lb_entries[indices[i]];
        float       rowY = innerTop + (float)(i * LB_PREVIEW_ROW_H);
        const char* trophy;
        char        rankStr[8];
        float       xRank, wRank, xTrophy, timeR, twTime, timeX;
        float       ty = rowY + 4.0f;

        if (i == 0) trophy = TROPHY_WR;
        else if (i == 1) trophy = TROPHY_2;
        else if (i == 2) trophy = TROPHY_3;
        else            trophy = TROPHY_N;

        snprintf(rankStr, sizeof(rankStr), "%2d", i + 1);

        if (i == 0)
            UiSetColor(&c, 220, 195, 110, 0x78);
        else if (i == 1)
            UiSetColor(&c, 200, 210, 225, 0x70);
        else if (i == 2)
            UiSetColor(&c, 190, 200, 218, 0x68);
        else
            UiSetColor(&c, 168, 188, 208, 0x60);

        xRank = panelX0 + 8.0f + slideX;
        wRank = UiTextWidthPx(rankStr);
        xTrophy = xRank + wRank + 6.0f;

        v.x = xRank;
        v.y = ty;
        v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)rankStr, LEFT_ALIGN, &v, &c, &uiFont);

        v.x = xTrophy;
        q = fontx_print_ascii(q, 0, (const unsigned char*)trophy, LEFT_ALIGN, &v, &c, &uiFont);

        timeR = panelX1 - 10.0f + slideX;
        twTime = UiTextWidthPx(e->time_display);
        timeX = timeR - twTime;
        v.x = timeX;
        v.y = ty;
        q = fontx_print_ascii(q, 0, (const unsigned char*)e->time_display, LEFT_ALIGN, &v, &c, &uiFont);
    }

    if (count > maxRows) {
        UiSetColor(&c, 100, 130, 160, 0x55);
        v.x = panelX0 + 8.0f + slideX;
        v.y = innerTop + (float)(show * LB_PREVIEW_ROW_H) + 1.0f;
        v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)"+more in X", LEFT_ALIGN, &v, &c, &uiFontHint);
    }

    (void)pulse;
    return q;
}

static void UiDrawLevelBrowser(int catIdx, int hwIdx, int lvCursor, int lvScroll, int pulse, int transSlide,
    int previewSlide)
{
    packet_t* packet;
    qword_t* q;
    rect_t    rect;
    color_t   c;
    vertex_t  v;
    int i;
    int j;
    float listTop = 100.0f;
    float hwTabBottom = 82.0f;
    int vrows = LB_VISIBLE_ROWS;
    float slideY = (float)(transSlide * 5);

    if (!uiReady) return;
    if (lb_category_count == 0) return;

    packet = packet_init(16384, PACKET_NORMAL);
    q = packet->data;

    q = draw_clear(q, 0, 0.0f, 0.0f, 640.0f, 448.0f, 8, 12, 18);
    q = UiTexAfterClear(q, 0, UI_TEX_BACKDROP_SLOT);

    /* Main panel */
    UiSetColor(&c, 18, 26, 36, 0x80);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 412.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    /* Header bar */
    UiSetColor(&c, 34, 53, 72, 0x40);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 58.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 214, 224, 233, 0x80);
    v.x = 52.0f; v.y = 42.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)"GHOST LEADERBOARD", LEFT_ALIGN, &v, &c, &uiFont);

    /* --- Category tabs (L1 / R1) --- */
    {
        float tabX = 36.0f;
        for (i = 0; i < lb_category_count; i++) {
            float tabW = (float)(strlen(lb_categories[i].name) * UI_FONT_CHAR_W + 16);
            if (i == catIdx) {
                UiSetColor(&c, 80, 185, 235, (unsigned char)(0x40 + (pulse & 0x1F)));
                UiSetRect(&rect, tabX, 58.0f, tabX + tabW, 78.0f, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 10, 10, 20, 0x80);
            }
            else {
                UiSetColor(&c, 34, 53, 72, 0x60);
                UiSetRect(&rect, tabX, 60.0f, tabX + tabW, 78.0f, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 140, 160, 180, 0x60);
            }
            if (i == catIdx)
                q = UiDrawFocusRing(q, 0, tabX, 58.0f, tabX + tabW, 78.0f, pulse);
            v.x = tabX + 10.0f; v.y = 62.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)lb_categories[i].name,
                LEFT_ALIGN, &v, &c, &uiFont);
            tabX += tabW + 4.0f;
        }
    }

    /* Accent line under category tabs */
    UiSetColor(&c, 80, 185, 235, (unsigned char)(0x20 + (pulse & 0x1F)));
    UiSetRect(&rect, 34.0f, 78.0f, 606.0f, 80.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    /* --- Homeworld tabs (Left / Right), wrapped rows --- */
    if (catIdx < lb_category_count) {
        LbCategory* cat = &lb_categories[catIdx];
        float tx[24], ty[24], tw[24];
        int hwN = 0;
        float sepY;
        float stickyTop;

        LbHomeworldTabLayout(catIdx, &listTop, &vrows, tx, ty, tw, &hwN, &hwTabBottom);
        (void)hwN;

        for (j = 0; j < cat->homeworld_count; j++) {
            const char* hwName = cat->homeworlds[j].name;
            float tabX = tx[j];
            float tabY = ty[j];
            float tabW = tw[j];
            float tabBottom = tabY + (float)LB_HW_ROW_H - 2.0f;

            if (j == hwIdx) {
                UiSetColor(&c, 72, 130, 195, 0x62);
                UiSetRect(&rect, tabX, tabY, tabX + tabW, tabBottom, &c);
                q = draw_rect_filled(q, 0, &rect);
                q = UiDrawFocusRing(q, 0, tabX, tabY, tabX + tabW, tabBottom, pulse);
                UiSetColor(&c, 200, 230, 255, 0x88);
            }
            else {
                UiSetColor(&c, 130, 150, 170, 0x50);
            }
            v.x = tabX + 10.0f; v.y = tabY + 6.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)hwName,
                LEFT_ALIGN, &v, &c, &uiFont);
        }

        /* Sticky section header: current homeworld + category (list scrolls under this label) */
        stickyTop = hwTabBottom + 6.0f + slideY;
        UiSetColor(&c, 22, 34, 48, 0x78);
        UiSetRect(&rect, 36.0f, stickyTop, 604.0f, stickyTop + (float)LB_STICKY_H, &c);
        q = draw_rect_filled(q, 0, &rect);
        if (hwIdx >= 0 && hwIdx < cat->homeworld_count) {
            UiSetColor(&c, 100, 200, 255, (unsigned char)(0x70 + (pulse & 0x1F)));
        }

        sepY = listTop + slideY - 3.0f;
        if (sepY < 90.0f) sepY = 90.0f;
        UiSetColor(&c, 50, 75, 100, 0x60);
        UiSetRect(&rect, 34.0f, sepY, 606.0f, sepY + 2.0f, &c);
        q = draw_rect_filled(q, 0, &rect);
    }
    else {
        UiSetColor(&c, 50, 75, 100, 0x60);
        UiSetRect(&rect, 34.0f, 96.0f, 606.0f, 98.0f, &c);
        q = draw_rect_filled(q, 0, &rect);
    }

    /* --- Level list --- */
    if (catIdx < lb_category_count && hwIdx < lb_categories[catIdx].homeworld_count) {
        LbHomeworld* hw = &lb_categories[catIdx].homeworlds[hwIdx];
        int visEnd = lvScroll + vrows;
        float drawListTop = listTop + slideY;
        float listBottom = drawListTop + (float)(vrows * LB_ROW_H);
        if (visEnd > hw->level_count) visEnd = hw->level_count;

        if (lvScroll > 0)
            q = UiDrawClipEdgeFade(q, 0, 36.0f, drawListTop, LB_LIST_INNER_R, 1, pulse);
        if (visEnd < hw->level_count)
            q = UiDrawClipEdgeFade(q, 0, 36.0f, listBottom, LB_LIST_INNER_R, 0, pulse);

        for (i = lvScroll; i < visEnd; i++) {
            float rowY = drawListTop + (float)((i - lvScroll) * LB_ROW_H);
            const char* lvName = hw->levels[i].name;

            if (i == lvCursor) {
                UiSetColor(&c, 80, 140, 200, 0x30);
                UiSetRect(&rect, 36.0f, rowY, LB_LIST_INNER_R, rowY + LB_ROW_H - 2, &c);
                q = draw_rect_filled(q, 0, &rect);
                UiSetColor(&c, 80, 185, 235, (unsigned char)(0x28 + (pulse & 0x1F)));
                UiSetRect(&rect, 36.0f, rowY, 40.0f, rowY + LB_ROW_H - 2, &c);
                q = draw_rect_filled(q, 0, &rect);
                q = UiDrawFocusRing(q, 0, 36.0f, rowY, LB_LIST_INNER_R, rowY + LB_ROW_H - 2, pulse);
                UiSetColor(&c, 200, 220, 240, 0x80);
            }
            else {
                UiSetColor(&c, 176, 190, 205, 0x60);
            }

            v.x = 50.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)lvName, LEFT_ALIGN, &v, &c, &uiFont);
        }

        /* Scroll arrows (left column) */
        UiSetColor(&c, 122, 150, 176, (unsigned char)(0x70 + (pulse & 0x1F)));
        if (lvScroll > 0) {
            v.x = LB_LIST_INNER_R - 10.0f; v.y = drawListTop + 4.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)"^", LEFT_ALIGN, &v, &c, &uiFont);
        }
        if (visEnd < hw->level_count) {
            v.x = LB_LIST_INNER_R - 10.0f;
            v.y = drawListTop + (float)((vrows - 1) * LB_ROW_H + 8);
            v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)"v", LEFT_ALIGN, &v, &c, &uiFont);
        }

        /* Column divider + times preview (skybox drawn after, same coordinates). */
        UiSetColor(&c, 45, 68, 92, 0x70);
        UiSetRect(&rect, LB_PREVIEW_SPLIT - 2.0f, drawListTop - 2.0f, LB_PREVIEW_SPLIT, LB_LEVEL_LIST_BOTTOM, &c);
        q = draw_rect_filled(q, 0, &rect);

        q = UiDrawLevelPreviewPanel(q, drawListTop, lb_categories[catIdx].name, hw->levels[lvCursor].name, pulse,
            previewSlide);
    }

    /* Hint bar */
    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 45.0f; v.y = 390.0f; v.z = 4;
    q = fontx_print_ascii(q, 0,
        (const unsigned char*)"L1/R1=Category | </>=Homeworld | Up/Dn=Level | X=View Times",
        LEFT_ALIGN, &v, &c, &uiFontHint);

    /* Skybox preview last so font prims are not drawn with a texture buffer still bound. */
    q = UiSkyPreviewDrawInPacket(q, 0);

    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    packet_free(packet);
}

/* =========================================================================
 * Times view — ranked ghosts for one level
 * Returns lb_entries index to download, or -1 to go back.
 * ========================================================================= */

static void UiDrawTimesView(const char* levelName, const char* catName,
    int* indices, int count,
    int cursor, int scroll, int pulse, int transSlide)
{
    packet_t* packet;
    qword_t* q;
    rect_t    rect;
    color_t   c;
    vertex_t  v;
    int i;
    int visEnd = scroll + LB_VISIBLE_ROWS;
    float slideY = (float)(transSlide * 5);
    float listY0 = 88.0f + slideY;
    float listBottom = listY0 + (float)(LB_VISIBLE_ROWS * LB_ROW_H);
    if (visEnd > count) visEnd = count;

    if (!uiReady) return;

    packet = packet_init(16384, PACKET_NORMAL);
    q = packet->data;

    q = draw_clear(q, 0, 0.0f, 0.0f, 640.0f, 448.0f, 8, 12, 18);
    q = UiTexAfterClear(q, 0, UI_TEX_BACKDROP_SLOT);

    UiSetColor(&c, 18, 26, 36, 0x80);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 412.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    /* Header */
    UiSetColor(&c, 34, 53, 72, 0x40);
    UiSetRect(&rect, 34.0f, 36.0f, 606.0f, 80.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 80, 185, 235, (unsigned char)(0x20 + (pulse & 0x1F)));
    UiSetRect(&rect, 34.0f, 80.0f, 606.0f, 82.0f, &c);
    q = draw_rect_filled(q, 0, &rect);

    UiSetColor(&c, 214, 224, 233, 0x80);
    v.x = 52.0f; v.y = 42.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)levelName, LEFT_ALIGN, &v, &c, &uiFont);

    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 52.0f; v.y = 60.0f; v.z = 4;
    q = fontx_print_ascii(q, 0, (const unsigned char*)catName, LEFT_ALIGN, &v, &c, &uiFont);

    if (count == 0) {
        UiSetColor(&c, 140, 160, 180, 0x60);
        v.x = 52.0f; v.y = 180.0f + slideY; v.z = 4;
        q = fontx_print_ascii(q, 0, (const unsigned char*)"No ghosts yet", LEFT_ALIGN, &v, &c, &uiFont);
    }
    else {
        if (scroll > 0)
            q = UiDrawClipEdgeFade(q, 0, 36.0f, listY0, 604.0f, 1, pulse);
        if (visEnd < count)
            q = UiDrawClipEdgeFade(q, 0, 36.0f, listBottom, 604.0f, 0, pulse);

        for (i = scroll; i < visEnd; i++) {
            LbEntry* e = &lb_entries[indices[i]];
            float rowY = listY0 + (float)((i - scroll) * LB_ROW_H);
            const char* trophy;
            char rankStr[8];
            char authDisp[26];

            if (i == 0) trophy = TROPHY_WR;
            else if (i == 1) trophy = TROPHY_2;
            else if (i == 2) trophy = TROPHY_3;
            else             trophy = TROPHY_N;

            snprintf(rankStr, sizeof(rankStr), "%2d", i + 1);
            strncpy(authDisp, e->author, 22);
            authDisp[22] = '\0';
            if (strlen(e->author) > 22) {
                authDisp[19] = '.';
                authDisp[20] = '.';
                authDisp[21] = '.';
                authDisp[22] = '\0';
            }

            if (i == cursor) {
                if (i == 0)
                    UiSetColor(&c, 180, 150, 40, 0x25);
                else
                    UiSetColor(&c, 80, 140, 200, 0x25);
                UiSetRect(&rect, 36.0f, rowY, 604.0f, rowY + LB_ROW_H - 2, &c);
                q = draw_rect_filled(q, 0, &rect);

                if (i == 0)
                    UiSetColor(&c, 235, 200, 80, (unsigned char)(0x28 + (pulse & 0x1F)));
                else
                    UiSetColor(&c, 80, 185, 235, (unsigned char)(0x28 + (pulse & 0x1F)));
                UiSetRect(&rect, 36.0f, rowY, 40.0f, rowY + LB_ROW_H - 2, &c);
                q = draw_rect_filled(q, 0, &rect);
                /* Ring stops short of the right so the time column is never clipped by the stroke. */
                q = UiDrawFocusRing(q, 0, 36.0f, rowY, 578.0f, rowY + LB_ROW_H - 2, pulse);
                UiSetColor(&c, 200, 220, 240, 0x80);
            }
            else {
                if (i == 0)
                    UiSetColor(&c, 220, 190, 100, 0x70);
                else
                    UiSetColor(&c, 176, 190, 205, 0x60);
            }

            v.x = 50.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)rankStr, LEFT_ALIGN, &v, &c, &uiFont);

            v.x = 74.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)trophy, LEFT_ALIGN, &v, &c, &uiFont);

            v.x = 124.0f; v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)authDisp, LEFT_ALIGN, &v, &c, &uiFont);

            v.x = LB_TIME_COL_RIGHT - UiTextWidthPx(e->time_display);
            v.y = rowY + 8.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)e->time_display, LEFT_ALIGN, &v, &c, &uiFont);
        }

        UiSetColor(&c, 122, 150, 176, (unsigned char)(0x70 + (pulse & 0x1F)));
        if (scroll > 0) {
            v.x = 582.0f; v.y = listY0 + 4.0f; v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)"^", LEFT_ALIGN, &v, &c, &uiFont);
        }
        if (visEnd < count) {
            v.x = 582.0f; v.y = listY0 + (float)((LB_VISIBLE_ROWS - 1) * LB_ROW_H + 8); v.z = 4;
            q = fontx_print_ascii(q, 0, (const unsigned char*)"v", LEFT_ALIGN, &v, &c, &uiFont);
        }
    }

    UiSetColor(&c, 122, 150, 176, 0x80);
    v.x = 52.0f; v.y = 390.0f; v.z = 4;
    q = fontx_print_ascii(q, 0,
        (const unsigned char*)"Up/Down = scroll   X = Download   O = Back",
        LEFT_ALIGN, &v, &c, &uiFont);

    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    packet_free(packet);
}

/* Returns lb_entries index to download, or -1 to go back to level browser. */
static int UiTimesView(const char* levelName, const char* catName,
    int* indices, int count)
{
    int cursor = 0, scroll = 0, pulse = 0;
    int transSlide = 3;
    unsigned short prev = 0xFFFF;
    for (;;) {
        unsigned short cur = ReadPadButtons();
        unsigned short edge = cur & ~prev;
        prev = cur;

        if (edge & PAD_CIRCLE) return -1;
        if (count > 0 && (edge & PAD_CROSS)) return indices[cursor];

        if (edge & PAD_UP) {
            if (cursor > 0) { cursor--; if (cursor < scroll) scroll = cursor; }
        }
        if (edge & PAD_DOWN) {
            if (cursor < count - 1) {
                cursor++;
                if (cursor >= scroll + LB_VISIBLE_ROWS)
                    scroll = cursor - LB_VISIBLE_ROWS + 1;
            }
        }
        UiDrawTimesView(levelName, catName, indices, count, cursor, scroll, pulse++, transSlide);
        if (transSlide > 0) transSlide--;
    }
}

/*
 * UiLevelBrowser
 * Returns lb_entries index of ghost to download,
 * or -1 to go back to mode select.
 */
static int UiLevelBrowser(void)
{
    int catIdx = 0, hwIdx = 0, lvCursor = 0, lvScroll = 0, pulse = 0;
    unsigned short prev = ReadPadButtons();
    int transSlide = 3;
    static int s_pbCat = -1;
    static int s_pbHw = -1;
    static int s_pbLv = -1;
    int        previewSlide = 0;

    if (lb_category_count == 0) return -1;

    for (;;) {
        LbCategory* cat;
        LbHomeworld* hw;
        float levelListTop = 100.0f;
        int visRows = LB_VISIBLE_ROWS;
        unsigned short cur = ReadPadButtons();
        unsigned short edge = cur & ~prev;
        prev = cur;

        cat = &lb_categories[catIdx];
        LbHomeworldTabLayout(catIdx, &levelListTop, &visRows, NULL, NULL, NULL, NULL, NULL);

        /* Category switch (L1 / R1) */
        if (edge & PAD_L1) {
            if (catIdx > 0) { catIdx--; hwIdx = 0; lvCursor = 0; lvScroll = 0; }
        }
        if (edge & PAD_R1) {
            if (catIdx < lb_category_count - 1) { catIdx++; hwIdx = 0; lvCursor = 0; lvScroll = 0; }
        }

        /* Homeworld switch (Left / Right) */
        if (edge & PAD_LEFT) {
            if (hwIdx > 0) { hwIdx--; lvCursor = 0; lvScroll = 0; }
        }
        if (edge & PAD_RIGHT) {
            if (hwIdx < cat->homeworld_count - 1) { hwIdx++; lvCursor = 0; lvScroll = 0; }
        }

        if (cat->homeworld_count > 0) {
            if (hwIdx < 0 || hwIdx >= cat->homeworld_count)
                hwIdx = 0;
        }
        else
            hwIdx = 0;
        hw = &cat->homeworlds[hwIdx];

        if (hw->level_count > 0) {
            int maxScroll = hw->level_count - visRows;
            if (maxScroll < 0) maxScroll = 0;
            if (lvScroll > maxScroll) lvScroll = maxScroll;
            if (lvCursor >= hw->level_count) lvCursor = hw->level_count - 1;
        }
        else {
            lvScroll = 0;
            lvCursor = 0;
        }

        /* Level list navigation */
        if (edge & PAD_UP) {
            if (lvCursor > 0) {
                lvCursor--;
                if (lvCursor < lvScroll) lvScroll = lvCursor;
            }
        }
        if (edge & PAD_DOWN) {
            if (lvCursor < hw->level_count - 1) {
                lvCursor++;
                if (lvCursor >= lvScroll + visRows)
                    lvScroll = lvCursor - visRows + 1;
            }
        }

        /* Preview slide after cursor / category / homeworld are final for this frame. */
        if (catIdx != s_pbCat || hwIdx != s_pbHw) {
            s_pbCat = catIdx;
            s_pbHw = hwIdx;
            s_pbLv = lvCursor;
            previewSlide = 5;
        }
        else if (hw->level_count > 0 && lvCursor != s_pbLv) {
            s_pbLv = lvCursor;
            previewSlide = 5;
        }
        else if (hw->level_count == 0) {
            s_pbLv = -1;
        }

        /* Enter times view */
        if (edge & PAD_CROSS && hw->level_count > 0) {
            const char* lvName = hw->levels[lvCursor].name;
            int indices[MAX_LB_ENTRIES];
            int count = GetGhostsForLevel(cat->name, lvName, indices, MAX_LB_ENTRIES);
            int selected = UiTimesView(lvName, cat->name, indices, count);
            if (selected >= 0) return selected; /* ghost chosen */
            /* Circle held across return would re-edge in this loop — resync baseline. */
            UiSyncPadPrev(&prev);
            transSlide = 3;
        }

        /* Back to mode select */
        if (edge & PAD_CIRCLE) {
            UiSkyPreviewUpdateForLevel(NULL);
            return -1;
        }

        if (hw->level_count > 0)
            UiSkyPreviewUpdateForLevel(hw->levels[lvCursor].name);
        else
            UiSkyPreviewUpdateForLevel(NULL);

        UiDrawLevelBrowser(catIdx, hwIdx, lvCursor, lvScroll, pulse++, transSlide, previewSlide);
        if (transSlide > 0) transSlide--;
        if (previewSlide > 0) previewSlide--;
    }
}
#endif /* NETWORK */

static int UiInit(void)
{
    packet_t* packet;
    qword_t* q;

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    uiFrame.width = 640;
    uiFrame.height = 448;
    uiFrame.psm = GS_PSM_32;
    uiFrame.mask = 0;
    uiFrame.address = graph_vram_allocate(uiFrame.width, uiFrame.height, uiFrame.psm, GRAPH_ALIGN_PAGE);

    uiZ.enable = 0;
    uiZ.method = ZTEST_METHOD_GREATER;
    uiZ.address = 0;
    uiZ.mask = 1;
    uiZ.zsm = 0;

    graph_initialize(uiFrame.address, uiFrame.width, uiFrame.height, uiFrame.psm, 0, 0);

    packet = packet_init(32, PACKET_NORMAL);
    q = packet->data;
    q = draw_setup_environment(q, 0, &uiFrame, &uiZ);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    dma_wait_fast();
    packet_free(packet);

    if (fontx_load("rom0:KROM", &uiFont, SINGLE_BYTE, 2, 2, 1) != 0)
        return -1;
    if (fontx_load("rom0:KROM", &uiFontHint, SINGLE_BYTE, 0, 2, 1) != 0) {
        fontx_unload(&uiFont);
        return -1;
    }

    UiTexturesInit();

    uiReady = 1;
    return 0;
}

static void UiShutdown(void)
{
    UiTexturesShutdown();
    if (uiReady) {
        fontx_unload(&uiFontHint);
        fontx_unload(&uiFont);
        uiReady = 0;
    }
    if (uiAudioReady) { audsrv_quit(); uiAudioReady = 0; }
}

static void UiWaitForButton(unsigned short mask, const char* title, const char* l1, const char* l2, const char* l3, const char* hint)
{
    int pulse = 0;
    while (ReadPadButtons() & mask) UiDrawFrame(title, l1, l2, l3, hint, pulse++);
    while (!(ReadPadButtons() & mask)) UiDrawFrame(title, l1, l2, l3, hint, pulse++);
}

static void UiWaitForButtonError(unsigned short mask, const char* title, const char* l1, const char* l2, const char* l3, const char* hint)
{
    int pulse = 0;
    while (ReadPadButtons() & mask) UiDrawErrorFrame(title, l1, l2, l3, hint, pulse++);
    while (!(ReadPadButtons() & mask)) UiDrawErrorFrame(title, l1, l2, l3, hint, pulse++);
}

/* =========================================================================
 * File scanning
 * ========================================================================= */

static int ScanGhostFiles(void)
{
    iox_dirent_t entry;
    int dd, n = 0;

    dd = fileXioDopen("mass:");
    if (dd < 0) dd = fileXioDopen("mass:/");
    if (dd < 0) return 0;

    while (fileXioDread(dd, &entry) > 0 && n < MAX_FILES) {
        int len = strlen(entry.name);
        if (len > 10 &&
            strncmp(entry.name, "ghost_", 6) == 0 &&
            strcmp(entry.name + len - 4, ".bin") == 0)
        {
            strncpy(fileNames[n], entry.name, 255);
            fileNames[n][255] = '\0';
            n++;
        }
    }

    fileXioDclose(dd);
    fileCount = n;
    return n;
}

/* =========================================================================
 * File selection loop
 * ========================================================================= */

static int UiFileSelect(void)
{
    int cursor = 0;
    int scrollOff = 0;
    int pulse = 0;
    unsigned short prev = ReadPadButtons();

    for (;;) {
        unsigned short cur = ReadPadButtons();
        unsigned short edge = cur & ~prev;
        prev = cur;

        if (edge & PAD_CROSS)  return cursor;
        if (edge & PAD_CIRCLE) return -1;

        if (edge & PAD_UP) {
            if (cursor > 0) {
                cursor--;
                if (cursor < scrollOff) scrollOff = cursor;
            }
        }
        if (edge & PAD_DOWN) {
            if (cursor < fileCount - 1) {
                cursor++;
                if (cursor >= scrollOff + VISIBLE_ROWS)
                    scrollOff = cursor - VISIBLE_ROWS + 1;
            }
        }

        UiDrawFileList(cursor, scrollOff, pulse++);
    }
}

/* =========================================================================
 * Ghost file loading and validation
 * ========================================================================= */

static int LoadGhostFromUSB(const char* filename)
{
    char path[280];
    int  fd, bytesRead;

    sprintf(path, "mass:%s", filename);
    fd = fileXioOpen(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    memset(ghostBuffer, 0, GHOST_REGION_SIZE);
    bytesRead = fileXioRead(fd, ghostBuffer, GHOST_REGION_SIZE);
    fileXioClose(fd);

    if (bytesRead < (int)sizeof(GhostHeader)) return -2;
    return 0;
}

static int GhostHeaderIsV2(const GhostHeader* h)
{
    return h->magic == (int)GHOST_V2_MAGIC &&
        h->version == GHOST_V2_VERSION &&
        h->headerSize == (int)sizeof(GhostHeader);
}

static int GhostHeaderSizeBytes(const GhostHeader* h)
{
    if (GhostHeaderIsV2(h))
        return h->headerSize;
    return (int)GHOST_LEGACY_HEADER_SIZE;
}

static int GhostDragonEventCount(const GhostHeader* h)
{
    if (!GhostHeaderIsV2(h))
        return 0;
    if (h->dragonEventCount < 0)
        return 0;
    if (h->dragonEventCount > GHOST_MAX_DRAGON_EVENTS)
        return GHOST_MAX_DRAGON_EVENTS;
    return h->dragonEventCount;
}

static int ValidateGhostHeader(GhostHeader* h)
{
    int headerSize = GhostHeaderSizeBytes(h);
    int maxFrames;
    int i;

    if (headerSize < (int)GHOST_LEGACY_HEADER_SIZE ||
        headerSize >(int)sizeof(GhostHeader))
        return 0;

    maxFrames = ((int)GHOST_REGION_SIZE - headerSize) / (int)GHOST_FRAME_SIZE;
    if (h->levelId < 10 || h->levelId > 64)          return 0;
    if (h->frameCount <= 0 || h->frameCount > maxFrames) return 0;
    if (h->frameCount > GHOST_MAX_TIME_FRAMES) return 0;
    if (h->finalTimeFrames <= 0 || h->finalTimeFrames > GHOST_MAX_TIME_FRAMES) return 0;

    if (GhostHeaderIsV2(h)) {
        if (h->dragonEventCount < 0 || h->dragonEventCount > GHOST_MAX_DRAGON_EVENTS)
            return 0;
        for (i = 0; i < h->dragonEventCount; i++) {
            if (h->dragonEvents[i].frameIndex < 0 ||
                h->dragonEvents[i].frameIndex > h->frameCount)
                return 0;
            if (h->dragonEvents[i].durationFrames < 0)
                return 0;
        }
    }
    return 1;
}

static void RunSaverFlow(int readResult, int usbReady)
{
    int ret;
    GhostHeader* header;
    char levelLine[96];
    char frameLine[96];
    char timeLine[96];
    unsigned short btn;
    int ft, ftMin, ftST, ftSO, ftMT, ftMH;

    if (readResult < 0) {
        UiPlayDing();
        UiWaitForButtonError(PAD_CROSS, "SAVE GHOST - ERROR", "Failed to read DECKARD before IOP reset.",
            "Ensure you are on a PS2 model 75k or above.", "Deckard hardware not detected.", "Press X");
        return;
    }

    if (!usbReady) {
        UiPlayDing();
        UiWaitForButtonError(PAD_CROSS, "USB NOT DETECTED", "No FAT32 drive was found.",
            "Insert USB and try Save Ghost again from the main menu.", "", "Press X");
        return;
    }

    header = (GhostHeader*)ghostBuffer;

    if (!ValidateGhostHeader(header)) {
        UiPlayDing();
        UiWaitForButtonError(PAD_CROSS, "NO VALID GHOST", "Replay header failed validation.",
            "Record a ghost and soft reset before launching this tool.", "", "Press X");
        return;
    }

    ft = header->finalTimeFrames;
    ftMin = (ft * 10) / 35892;
    ftST = ((ft * 10) % 35892) / 5982;
    ftSO = ((ft * 100) % 59820) / 5982;
    ftMT = ((ft * 1000) % 59820) / 5982;
    ftMH = ((ft * 10000) % 59820) / 5982;

    sprintf(levelLine, "Level: %s (ID %d)", GetLevelName(header->levelId), header->levelId);
    sprintf(frameLine, "Frames: %d | %s | Dragons: %d",
        header->frameCount,
        GhostHeaderIsV2(header) ? "V2" : "V1",
        GhostDragonEventCount(header));
    sprintf(timeLine, "Final Time: %d:%d%d.%d%d", ftMin, ftST, ftSO, ftMT, ftMH);
    UiDrawFrame("SAVE GHOST", levelLine, frameLine, timeLine,
        "Review data. X = write, O = back", 18);
    btn = WaitForEither(PAD_CROSS, PAD_CIRCLE);
    if (btn == PAD_CIRCLE) {
        UiPlayDing();
        return;
    }
    UiPlayDing();

    BuildGhostFilename(header->levelId);

    ret = SaveToUSB(ghostBuffer, (int)GHOST_REGION_SIZE, ghostFilePath);

    if (ret == 0) {
        UiPlayDing();
        UiWaitForButton(PAD_CROSS, "SAVE COMPLETE", "Ghost file written to USB.",
            ghostFilePath + 5, "Press X for the main menu.", "Press X");
        return;
    }

    UiPlayDing();
    if (ret == -1)
        UiWaitForButtonError(PAD_CROSS, "SAVE ERROR", "Could not open destination file.",
            "Check FAT32 format and write protection.", "", "Press X");
    else
        UiWaitForButtonError(PAD_CROSS, "SAVE ERROR", "Write was incomplete.",
            "USB may be full or unstable.", "", "Press X");
}

/* =========================================================================
 * IOP write via ghostwr RPC
 * ========================================================================= */

static int WriteGhostDataViaRpc(void)
{
    unsigned int offset = GHOST_BOOT_BYTES;

    while (offset < (GHOST_BOOT_BYTES + GHOST_REGION_SIZE)) {
        unsigned int ghostOffset = offset - GHOST_BOOT_BYTES;
        unsigned int chunk = (GHOST_BOOT_BYTES + GHOST_REGION_SIZE) - offset;
        if (chunk > WR_CHUNK_SIZE) chunk = WR_CHUNK_SIZE;

        wrSendBuf.offset = offset;
        wrSendBuf.size = chunk;
        memcpy(wrSendBuf.data, ghostBuffer + ghostOffset, chunk);
        FlushCache(0);

        if (SifCallRpc(&ghostRpc, 0, 0,
            &wrSendBuf, sizeof(GhostWriteReq),
            wrRecvBuf, sizeof(wrRecvBuf),
            NULL, NULL) < 0)
            return -1;

        offset += chunk;
    }

    return 0;
}

static int WriteGhostBootRequestViaRpc(int targetLevelId)
{
    unsigned char* d = wrSendBuf.data;

    GhostBootPackDeckardWordLe(d + 0, GHOST_BOOT_MAGIC);
    GhostBootPackDeckardWordLe(d + 4, GHOST_BOOT_VERSION);
    GhostBootPackDeckardWordLe(d + 8, GHOST_BOOT_FLAG_FLYIN_REQ);
    GhostBootPackDeckardWordLe(d + 12, (unsigned int)targetLevelId);
    memset(d + 16, 0, 16);

    wrSendBuf.offset = 0;
    wrSendBuf.size = GHOST_BOOT_BYTES;
    FlushCache(0);

    if (SifCallRpc(&ghostRpc, 0, 0,
        &wrSendBuf, sizeof(GhostWriteReq),
        wrRecvBuf, sizeof(wrRecvBuf),
        NULL, NULL) < 0)
        return -1;

    return 0;
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char* argv[])
{
    int ret, modRes;
    int bindRetries;
    int mode;
    int readResult = -1;
    GhostHeader* header;

    InitIopClients();

    init_scr();
    if (UiInit() < 0) goto hang;

    UiDrawFrame("GHOST TOOL", "Initializing...", "Loading controller modules.", "Please wait.", "Deckard / Spyro 1 utility", 0);
    if (InitPadRuntime() < 0) goto hang;

    {
        int ghostwrLoaded = 0, usbLoaded = 0, audioLoaded = 0;
        #ifdef NETWORK
        int netLoaded = 0;
        #endif

        if (!audioLoaded) {
            UiAudioInit();
            audioLoaded = 1;
        }

    root_menu:
        if (UiSelectSaveOrLoad() == 0) {
            UiDrawFrame("SAVE GHOST", "Reading Deckard memory...",
                "Copying ghost data into EE RAM.", "Please wait.", "Preparing save", 4);
            readResult = SaverPrereadDeckard();

            UiDrawFrame("SAVE GHOST", "Preparing USB write...",
                "Using existing mass: mount when available.", "Please wait.", "Preparing save", 5);
            if (LoadUsbStackIfNeeded(&usbLoaded) < 0) goto hang;
            {
                int usbReady = WaitForUsbMass("SAVE GHOST");
                if (usbReady < 0) goto root_menu;
                RunSaverFlow(readResult, usbReady);
            }
            goto root_menu;
        }

        #if defined(NETWORK) && defined(GHOST_LOADER_DEBUG_UI)
        mode_select:
        #endif
        mode = UiSelectMode(); /* 0 = USB, 1 = Network, -1 = back */
        if (mode < 0) goto root_menu;

        if (mode == 0) {

            if (LoadUsbStackIfNeeded(&usbLoaded) < 0) goto hang;

            if (!ghostwrLoaded) {
                ret = SifExecModuleBuffer(ghostwr_irx, size_ghostwr_irx, 0, NULL, &modRes);
                if (ret < 0) {
                    UiWaitForButtonError(PAD_CROSS, "GHOST LOADER - ERROR", "Failed to load ghostwr.irx.",
                        "IOP write module could not start.", "Press X to return to the main menu.", "Press X");
                    goto root_menu;
                }
                DelayMs(200);
                memset(&ghostRpc, 0, sizeof(ghostRpc));
                ret = -1;
                for (bindRetries = 0; bindRetries < 50; bindRetries++) {
                    if (SifBindRpc(&ghostRpc, GHOST_RPC_WR_ID, 0) >= 0 && ghostRpc.server != NULL)
                    {
                        ret = 0; break;
                    }
                    DelayMs(50);
                }
                if (ret < 0) {
                    UiWaitForButtonError(PAD_CROSS, "GHOST LOADER - ERROR", "ghostwr RPC bind failed.",
                        "IOP write module did not respond.", "Press X to return to the main menu.", "Press X");
                    goto root_menu;
                }
                ghostwrLoaded = 1;
            }

            {
                int usbReady = WaitForUsbMass("GHOST LOADER");
                if (usbReady < 0) goto root_menu;
                if (usbReady == 0) {
                    UiPlayDing();
                    UiWaitForButtonError(PAD_CROSS, "USB NOT DETECTED", "No FAT32 drive was found.",
                        "Insert USB and try Load Ghost again.", "", "Press X");
                    goto root_menu;
                }
            }

            UiDrawFrame("GHOST LOADER", "Scanning USB for ghost files...",
                "Looking for ghost_*.bin on mass:", "Please wait.", "Scanning", 4);
            if (ScanGhostFiles() == 0) {
                UiPlayDing();
                UiWaitForButtonError(PAD_CROSS, "NO FILES FOUND", "No ghost_*.bin files on USB drive.",
                    "Use Save Ghost first, or add files to the drive.", "", "Press X");
                goto root_menu;
            }

            {
                int selected = UiFileSelect();
                if (selected < 0) { goto root_menu; }
                UiPlayDing();
                UiDrawFrame("GHOST LOADER", "Reading file from USB...", fileNames[selected], "Please wait.", "Loading", 8);
                ret = LoadGhostFromUSB(fileNames[selected]);
                if (ret < 0) {
                    UiWaitForButtonError(PAD_CROSS, "READ ERROR", "Could not read ghost file from USB.",
                        "File may be corrupt or inaccessible.", "", "Press X");
                    goto root_menu;
                }
            }

        }
        #ifdef NETWORK
        else {

            #ifndef GHOST_LOADER_DEBUG_UI
                        /* USB + fileXio so mass:ghost_server.txt can override the default leaderboard host. */
            if (LoadUsbStackIfNeeded(&usbLoaded) < 0) goto hang;
            NetLoadServerConfig();
            #endif

            if (!ghostwrLoaded) {
                ret = SifExecModuleBuffer(ghostwr_irx, size_ghostwr_irx, 0, NULL, &modRes);
                if (ret < 0) {
                    UiWaitForButtonError(PAD_CROSS, "GHOST LOADER - ERROR", "Failed to load ghostwr.irx.",
                        "IOP write module could not start.", "Press X to return to the main menu.", "Press X");
                    goto root_menu;
                }
                DelayMs(200);
                memset(&ghostRpc, 0, sizeof(ghostRpc));
                ret = -1;
                for (bindRetries = 0; bindRetries < 50; bindRetries++) {
                    if (SifBindRpc(&ghostRpc, GHOST_RPC_WR_ID, 0) >= 0 && ghostRpc.server != NULL)
                    {
                        ret = 0; break;
                    }
                    DelayMs(50);
                }
                if (ret < 0) {
                    UiWaitForButtonError(PAD_CROSS, "GHOST LOADER - ERROR", "ghostwr RPC bind failed.",
                        "IOP write module did not respond.", "Press X to return to the main menu.", "Press X");
                    goto root_menu;
                }
                ghostwrLoaded = 1;
            }

            if (!netLoaded) {
                #ifdef GHOST_LOADER_DEBUG_UI
                netLoaded = 1;
                #else
                ret = NetInit();
                if (ret == -1) {
                    UiPlayDing();
                    UiWaitForButtonError(PAD_CROSS, "NETWORK ERROR", "Failed to load network IOP modules.",
                        "", "", "Press X");
                    goto root_menu;
                }
                if (ret == -2) {
                    char srvLine[88];
                    UiPlayDing();
                    snprintf(srvLine, sizeof(srvLine), "Server: %s:%d", net_server_host, net_server_port);
                    UiWaitForButtonError(PAD_CROSS, "NETWORK ERROR", "DHCP timed out. No IP received.",
                        "Check ethernet cable and router.", srvLine, "Press X");
                    goto root_menu;
                }
                netLoaded = 1;
                #endif
            }

            #ifdef GHOST_LOADER_DEBUG_UI
            LbLoadDebugLeaderboardStub();
            ret = lb_count;
            if (ret <= 0) {
                UiPlayDing();
                UiWaitForButtonError(PAD_CROSS, "DEBUG UI ERROR", "Stub leaderboard failed to parse.",
                    "Rebuild with DEBUG_UI=0 and report.", "", "Press X");
                goto root_menu;
            }
            #else
            ret = FetchLeaderboard();
            if (ret < 0) {
                char srvLine[88];
                UiPlayDing();
                snprintf(srvLine, sizeof(srvLine), "Server: %s:%d", net_server_host, net_server_port);
                UiWaitForButtonError(PAD_CROSS, "NETWORK ERROR", "Could not connect to leaderboard.",
                    net_debug_msg[0] ? net_debug_msg
                    : "Try again later, or reach out to Composer on Discord.",
                    srvLine, "Press X");
                goto root_menu;
            }
            #endif

            {
                int selected = UiLevelBrowser();
                if (selected < 0) { goto root_menu; }
                UiPlayDing();

                #ifdef GHOST_LOADER_DEBUG_UI
                UiDrawFrame("DEBUG UI (GHOST_LOADER_DEBUG_UI)",
                    "No download — TCP/IP and HTTP are disabled in this build.",
                    "Rebuild without DEBUG_UI=1 for real network.",
                    "Returning to mode select.", "Emulator UI iteration", 0);
                UiWaitForButton(PAD_CROSS, "DEBUG UI (GHOST_LOADER_DEBUG_UI)",
                    "No download — TCP/IP and HTTP are disabled in this build.",
                    "Rebuild without DEBUG_UI=1 for real network.",
                    "Returning to mode select.", "Press X");
                goto mode_select;
                #else
                {
                    char dlLine[64];
                    snprintf(dlLine, sizeof(dlLine), "%.52s", lb_entries[selected].filename);
                    UiDrawFrame("GHOST LOADER - Network", "Downloading ghost file...",
                        dlLine, "Please wait.", "Fetching from server", 12);
                }

                ret = (int)DownloadGhost(selected, ghostBuffer, (int)GHOST_REGION_SIZE);
                if (ret < (int)sizeof(GhostHeader)) {
                    UiPlayDing();
                    UiWaitForButtonError(PAD_CROSS, "DOWNLOAD ERROR", "Ghost file download failed.",
                        net_debug_msg[0] ? net_debug_msg
                        : "Server may be unreachable or file is missing.",
                        "Check server and index.json.", "Press X");
                    goto root_menu;
                }
                #endif
            }
        }
        #endif /* NETWORK */

        header = (GhostHeader*)ghostBuffer;

        if (!ValidateGhostHeader(header)) {
            char badLine[64];
            sprintf(badLine, "levelId=%d frames=%d", header->levelId, header->frameCount);
            UiPlayDing();
            UiWaitForButtonError(PAD_CROSS, "INVALID GHOST FILE", "Header validation failed.",
                badLine, "File may be corrupt or wrong format.", "Press X");
            goto root_menu;
        }

        {
            int ft = header->finalTimeFrames;
            int ftMin = (ft * 10) / 35892;
            int ftST = ((ft * 10) % 35892) / 5982;
            int ftSO = ((ft * 100) % 59820) / 5982;
            int ftMT = ((ft * 1000) % 59820) / 5982;
            int ftMH = ((ft * 10000) % 59820) / 5982;

            char levelLine[64], frameLine[64], timeLine[64];
            sprintf(levelLine, "Level: %s (ID %d)", GetLevelName(header->levelId), header->levelId);
            sprintf(frameLine, "Frames: %d | %s | Dragons: %d",
                header->frameCount,
                GhostHeaderIsV2(header) ? "V2" : "V1",
                GhostDragonEventCount(header));
            sprintf(timeLine, "Time: %d:%d%d.%d%d", ftMin, ftST, ftSO, ftMT, ftMH);

            UiDrawFrame("GHOST FILE INFO", levelLine, frameLine, timeLine,
                "Review data. X = load, O = back", 18);
            if (WaitForEither(PAD_CROSS, PAD_CIRCLE) == PAD_CIRCLE) {
                UiPlayDing();
                goto root_menu;
            }
            UiPlayDing();
        }

        UiDrawFrame("GHOST LOADER", "Writing ghost data to IOP...",
            "Writing boot handshake and ghost payload.",
            "Sending via ghostwr RPC in 4KB chunks.",
            "Writing Into Memory", 14);

        ret = WriteGhostBootRequestViaRpc(header->levelId);
        if (ret < 0) {
            UiPlayDing();
            UiWaitForButtonError(PAD_CROSS, "WRITE ERROR", "Could not write boot handshake.",
                "ghostwr did not respond correctly.", "", "Press X");
            goto root_menu;
        }

        ret = WriteGhostDataViaRpc();
        if (ret < 0) {
            UiPlayDing();
            UiWaitForButtonError(PAD_CROSS, "WRITE ERROR", "RPC call failed during write.",
                "ghostwr did not respond correctly.", "", "Press X");
            goto root_menu;
        }

        UiPlayDing();
        UiWaitForButton(PAD_CROSS, "LOAD COMPLETE",
            "Ghost loaded into ram.",
            "Insert Spyro 1 Practice Rom, then press X",
            "",
            "Press X");
        UiShutdown();
        return 0;
    }

    return 0;

hang:
    if (uiReady && uiPadReady) {
        UiPlayDing();
        UiWaitForButtonError(PAD_CROSS, "FATAL ERROR",
            "A required module failed to load.",
            "Initialization did not complete.",
            "Relaunch from uLaunchELF and retry.",
            "Press X to exit");
        UiShutdown(); return 1;
    }
    if (uiReady) {
        UiDrawErrorFrame("FATAL ERROR",
            "A required module failed to load.",
            "Initialization did not complete.",
            "Relaunch from uLaunchELF and retry.",
            "Restart required", 10);
    }
    UiShutdown();
    SleepThread();
    return 1;
}
