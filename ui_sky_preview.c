/*
 * Corner skybox preview for leaderboard level browser.
 */

#include <string.h>

#include <kernel.h>
#include <dma.h>
#include <packet.h>
#include <graph.h>
#include <graph_vram.h>
#include <gs_psm.h>
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>

#include "net.h"
#include "skybox_embed.h"
#include "ui_sky_preview.h"

static void sp_set_color(color_t* c, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    c->r = r;
    c->g = g;
    c->b = b;
    c->a = a;
    c->q = 1.0f;
}

static void sp_set_rect(rect_t* r, float x0, float y0, float x1, float y1, color_t* c)
{
    r->v0.x = x0;
    r->v0.y = y0;
    r->v0.z = 2;
    r->v1.x = x1;
    r->v1.y = y1;
    r->v1.z = 2;
    r->color = *c;
}

#define SKY_GPU 256
#define SKY_RAW_BYTES ((SKY_GPU) * (SKY_GPU) * 4)

static int              g_sky_ready;
static int              g_sky_vram_addr = -1;
static texbuffer_t      g_sky_tex;
static clutbuffer_t     g_sky_clut;
static lod_t            g_sky_lod;
static char             g_sky_last_slug[64];

static const SkyboxEmbed* sky_embed_find(const char* slug)
{
    int i;

    if (!slug || !slug[0] || g_skybox_embed_count <= 0)
        return NULL;

    for (i = 0; i < g_skybox_embed_count; i++) {
        if (g_skybox_embed[i].slug && strcmp(g_skybox_embed[i].slug, slug) == 0)
            return &g_skybox_embed[i];
    }
    return NULL;
}

static void sky_zero_texbuf(void)
{
    memset(&g_sky_tex, 0, sizeof(g_sky_tex));
    memset(&g_sky_clut, 0, sizeof(g_sky_clut));
    memset(&g_sky_lod, 0, sizeof(g_sky_lod));

    g_sky_clut.storage_mode = CLUT_STORAGE_MODE1;
    g_sky_clut.start = 0;
    g_sky_clut.psm = GS_PSM_32;
    g_sky_clut.load_method = CLUT_NO_LOAD;
    g_sky_clut.address = 0;

    g_sky_lod.calculation = LOD_USE_K;
    g_sky_lod.max_level = 0;
    g_sky_lod.mag_filter = LOD_MAG_LINEAR;
    g_sky_lod.min_filter = LOD_MIN_LINEAR;
    g_sky_lod.l = 0;
    g_sky_lod.k = 0;
}

void UiSkyPreviewUpdateForLevel(const char* display_name_or_null)
{
    char                    slug[80];
    const SkyboxEmbed* em;

    if (!display_name_or_null || !display_name_or_null[0]) {
        g_sky_ready = 0;
        g_sky_last_slug[0] = '\0';
        return;
    }

    LbLevelNameToSlug(display_name_or_null, slug, sizeof(slug));
    if (!slug[0])
        return;

    if (strcmp(g_sky_last_slug, slug) == 0)
        return;

    em = sky_embed_find(slug);
    if (!em || !em->data || em->size_bytes != (unsigned int)SKY_RAW_BYTES) {
        g_sky_ready = 0;
        g_sky_last_slug[0] = '\0';
        return;
    }

    if (g_sky_vram_addr < 0) {
        g_sky_vram_addr = graph_vram_allocate(SKY_GPU, SKY_GPU, GS_PSM_32, GRAPH_ALIGN_BLOCK);
        if (g_sky_vram_addr < 0) {
            g_sky_ready = 0;
            return;
        }
        sky_zero_texbuf();
        g_sky_tex.width = (unsigned int)SKY_GPU;
        g_sky_tex.psm = GS_PSM_32;
        g_sky_tex.address = (unsigned int)g_sky_vram_addr;
        g_sky_tex.info.width = draw_log2((unsigned int)SKY_GPU);
        g_sky_tex.info.height = draw_log2((unsigned int)SKY_GPU);
        g_sky_tex.info.components = TEXTURE_COMPONENTS_RGBA;
        g_sky_tex.info.function = TEXTURE_FUNCTION_DECAL;
    }

    {
        /* draw_texture_transfer(256^2) generates a long GIF chain; 128 qwords is far too
         * small and will overrun the buffer — shows up as crash the first time you change
         * level to one with a matching embedded sky. */
        packet_t* pkt = packet_init(16384, PACKET_NORMAL);
        qword_t* q = pkt->data;
        q = draw_texture_transfer(q, (void*)em->data, SKY_GPU, SKY_GPU, GS_PSM_32,
            g_sky_vram_addr, SKY_GPU);
        q = draw_texture_flush(q);
        dma_wait_fast();
        dma_channel_send_chain(DMA_CHANNEL_GIF, pkt->data, (int)(q - pkt->data), 0, 0);
        dma_wait_fast();
        packet_free(pkt);
    }

    strncpy(g_sky_last_slug, slug, sizeof(g_sky_last_slug) - 1);
    g_sky_last_slug[sizeof(g_sky_last_slug) - 1] = '\0';
    g_sky_ready = 1;
}

qword_t* UiSkyPreviewDrawInPacket(qword_t* q, int context)
{
    rect_t    r;
    color_t   c;
    texrect_t tr;
    float     bx0 = 448.0f;
    float     by0 = 248.0f;
    float     bx1 = 602.0f;
    float     by1 = 382.0f;
    float     pad = 4.0f;

    if (!g_sky_ready || g_sky_vram_addr < 0)
        return q;

    /* Outer bezel */
    sp_set_color(&c, 8, 12, 20, 0xE0);
    sp_set_rect(&r, bx0 - 2.0f, by0 - 2.0f, bx1 + 2.0f, by1 + 2.0f, &c);
    q = draw_rect_filled(q, context, &r);
    sp_set_color(&c, 40, 60, 82, 0xF0);
    sp_set_rect(&r, bx0, by0, bx1, by1, &c);
    q = draw_rect_filled(q, context, &r);

    tr.v0.x = bx0 + pad;
    tr.v0.y = by0 + pad;
    tr.v0.z = 8;
    tr.v1.x = bx1 - pad;
    tr.v1.y = by1 - pad;
    tr.v1.z = 8;
    /* texrect u,v are in texels: ftoi4() * 16 => GS UV; 0..1 only covers one texel. */
    tr.t0.u = 0.0f;
    tr.t0.v = 0.0f;
    tr.t1.u = (float)SKY_GPU;
    tr.t1.v = (float)SKY_GPU;
    tr.color.r = 0xFF;
    tr.color.g = 0xFF;
    tr.color.b = 0xFF;
    tr.color.a = 0xFF;
    tr.color.q = 1.0f;

    q = draw_texture_sampling(q, context, &g_sky_lod);
    q = draw_texturebuffer(q, context, &g_sky_tex, &g_sky_clut);
    q = draw_rect_textured(q, context, &tr);
    return q;
}
