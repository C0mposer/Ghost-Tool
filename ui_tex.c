/*
 * Embedded UI textures. Raw blobs from scripts/png_to_gs_rgba32.py + bin2s.
 */

#include <stddef.h>
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

#include "ui_tex.h"

#define UI_TEX_MAX 32

static int           g_tex_ready;
static int           g_tex_uploaded[UI_TEX_MAX];
static int           g_tex_addr[UI_TEX_MAX];
static texbuffer_t   g_tex_buf[UI_TEX_MAX];
static clutbuffer_t  g_tex_clut;
static lod_t         g_tex_lod;

void UiTexturesInit(void)
{
    int i;

    if (g_tex_ready)
        return;

    memset(g_tex_uploaded, 0, sizeof(g_tex_uploaded));
    memset(g_tex_addr, 0, sizeof(g_tex_addr));
    memset(g_tex_buf, 0, sizeof(g_tex_buf));

    memset(&g_tex_clut, 0, sizeof(g_tex_clut));
    g_tex_clut.storage_mode = CLUT_STORAGE_MODE1;
    g_tex_clut.start = 0;
    g_tex_clut.psm = GS_PSM_32;
    g_tex_clut.load_method = CLUT_NO_LOAD;
    g_tex_clut.address = 0;

    g_tex_lod.calculation = LOD_USE_K;
    g_tex_lod.max_level = 0;
    g_tex_lod.mag_filter = LOD_MAG_LINEAR;
    g_tex_lod.min_filter = LOD_MIN_LINEAR;
    g_tex_lod.l = 0;
    g_tex_lod.k = 0;

    if (g_ui_tex_table_count <= 0)
        goto done;

    {
        int n = g_ui_tex_table_count;
        if (n > UI_TEX_MAX)
            n = UI_TEX_MAX;

        for (i = 0; i < n; i++) {
            const UiTexDesc* d = &g_ui_tex_table[i];
            packet_t* pkt;
            qword_t* q;
            int       addr;
            unsigned int need;

            if (!d->data || d->gpu_w <= 0 || d->gpu_h <= 0)
                continue;

            need = (unsigned int)(d->gpu_w * d->gpu_h * 4);
            if (d->size_bytes < need)
                continue;

            addr = graph_vram_allocate(d->gpu_w, d->gpu_h, GS_PSM_32, GRAPH_ALIGN_BLOCK);
            if (addr < 0)
                continue;

            pkt = packet_init(128, PACKET_NORMAL);
            q = pkt->data;
            q = draw_texture_transfer(q, (void*)d->data, d->gpu_w, d->gpu_h, GS_PSM_32, addr, d->gpu_w);
            q = draw_texture_flush(q);
            dma_wait_fast();
            dma_channel_send_chain(DMA_CHANNEL_GIF, pkt->data, (int)(q - pkt->data), 0, 0);
            dma_wait_fast();
            packet_free(pkt);

            g_tex_addr[i] = addr;
            g_tex_buf[i].width = (unsigned int)d->gpu_w;
            g_tex_buf[i].psm = GS_PSM_32;
            g_tex_buf[i].address = (unsigned int)addr;
            g_tex_buf[i].info.width = draw_log2((unsigned int)d->gpu_w);
            g_tex_buf[i].info.height = draw_log2((unsigned int)d->gpu_h);
            g_tex_buf[i].info.components = TEXTURE_COMPONENTS_RGBA;
            g_tex_buf[i].info.function = TEXTURE_FUNCTION_DECAL;
            g_tex_uploaded[i] = 1;
        }
    }

done:
    g_tex_ready = 1;
}

void UiTexturesShutdown(void)
{
    memset(g_tex_uploaded, 0, sizeof(g_tex_uploaded));
    g_tex_ready = 0;
}

int UiTexturesAny(void)
{
    return g_tex_ready && g_ui_tex_table_count > 0 && g_tex_uploaded[0];
}

qword_t* UiTexAfterClear(qword_t* q, int context, int slot)
{
    texrect_t   tr;
    color_t     col;

    if (!UiTexturesAny() || slot < 0 || slot >= g_ui_tex_table_count || !g_tex_uploaded[slot])
        return q;

    {
        const UiTexDesc* d = &g_ui_tex_table[slot];

        col.r = 0xFF;
        col.g = 0xFF;
        col.b = 0xFF;
        col.a = 0xFF;
        col.q = 1.0f;

        tr.v0.x = 0.0f;
        tr.v0.y = 0.0f;
        tr.v0.z = 1;
        tr.v1.x = 640.0f;
        tr.v1.y = 448.0f;
        tr.v1.z = 1;
        tr.t0.u = 0.0f;
        tr.t0.v = 0.0f;
        /* Texel-space UV (see ui_sky_preview.c); not normalized 0..1. */
        tr.t1.u = (float)d->w;
        tr.t1.v = (float)d->h;
        tr.color = col;
    }

    q = draw_texture_sampling(q, context, &g_tex_lod);
    q = draw_texturebuffer(q, context, &g_tex_buf[slot], &g_tex_clut);
    q = draw_rect_textured(q, context, &tr);
    return q;
}
