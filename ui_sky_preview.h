#ifndef UI_SKY_PREVIEW_H
#define UI_SKY_PREVIEW_H

#include <packet.h>

/*
 * Leaderboard level browser: fetch /skybox/<slug>.raw when the focused level
 * changes (slug from index.json display name via LbLevelNameToSlug).
 */
void UiSkyPreviewUpdateForLevel(const char *display_name_or_null);

/* Append preview panel + textured quad to the current GIF packet (call before draw_finish). */
qword_t *UiSkyPreviewDrawInPacket(qword_t *q, int context);

#endif
