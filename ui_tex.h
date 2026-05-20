#ifndef UI_TEX_H
#define UI_TEX_H

#include <packet.h>

/* One embedded texture after PNG -> RAW conversion (see scripts/png_to_gs_rgba32.py). */
typedef struct UiTexDesc {
    const unsigned char *data;
    unsigned int         size_bytes;
    int                  w;      /* logical width (visible) */
    int                  h;      /* logical height */
    int                  gpu_w; /* power-of-two buffer width */
    int                  gpu_h; /* power-of-two buffer height */
} UiTexDesc;

extern const UiTexDesc g_ui_tex_table[];
extern const int       g_ui_tex_table_count;

/* Upload all embedded textures to GS VRAM (call once after DMA+graph init). */
void UiTexturesInit(void);

/* Optional teardown (VRAM is not freed on PS2 SDK stub; safe to skip). */
void UiTexturesShutdown(void);

/* Returns 1 if at least one texture is available (slot 0 usable as backdrop). */
int UiTexturesAny(void);

/*
 * Append a full-frame texturized quad for g_ui_tex_table[slot] after draw_clear.
 * Uses logical w/h for UV; respects gpu_w/gpu_h padding.
 */
qword_t *UiTexAfterClear(qword_t *q, int context, int slot);

#endif /* UI_TEX_H */
