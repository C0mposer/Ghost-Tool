#ifndef SKYBOX_EMBED_H
#define SKYBOX_EMBED_H

/* Built from ghost-server/skybox .raw blobs at link time (scripts/gen_skybox_table.py). */
typedef struct SkyboxEmbed {
    const char          *slug;
    const unsigned char *data;
    unsigned int         size_bytes;
} SkyboxEmbed;

extern const SkyboxEmbed g_skybox_embed[];
extern const int         g_skybox_embed_count;

#endif
