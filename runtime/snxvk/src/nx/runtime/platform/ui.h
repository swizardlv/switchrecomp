/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game's picture: the on-screen controls (vpad.c) and the game's menu (overlay.c),
   as quads of one atlas blended over the frame. The atlas is dist/controls/controls.png with the menu's 8x16 font
   and a white block for plain fills in the places its header keeps for them, and under it the picture of the user
   playing, which the menu's head shows by their nickname */
#ifndef SNX_UI_H
#define SNX_UI_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    float x, y, w, h; /* where, in the pixels of the picture drawn on */
    u16 u, v, uw, vh; /* the atlas texels drawn there */
    u32 rgba;         /* each texel times this colour, R G B A from the low byte */
    u8 turn;          /* the texels turned this many quarters clockwise */
    u8 sharp;         /* the nearest texel, not a blend: the font at whole steps */
} SnxUiQuad;
typedef struct {
    SnxUiQuad* q;
    int n, max; /* quads past max are dropped */
} SnxUiList;

/* a sprite of controls_atlas.h at alpha (0 to 255) */
void ui_sprite(SnxUiList* l, int sprite, float x, float y, float w, float h, u32 alpha, int turn);
/* a plain rectangle */
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba);
/* the picture of the user playing (common/utilities/users.h), side pixels square at x, y: drawn SNX_UI_PICTURE texels
   square in the YoRHa theme's [head] picture shape, square or round (the rest transparent); 0 when the theme shows
   none, which draws nothing */
#define SNX_UI_PICTURE 96
int ui_picture(SnxUiList* l, float x, float y, float side);
/* ASCII text, 0xFE the font's square, in 8x16 glyphs at a whole scale; returns the width drawn */
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba);

/* The atlas, each texel the bytes R G B A (a little-endian u32) with alpha premultiplied, made from the sprites'
   image when the window opens (sdl3.c decodes it to the same bytes, SDL's RGBA32), the users read by then, and freed
   by ui_atlas_done once the renderer has it, both under ui_lock. It is the image's width and SNX_UI_PICTURE rows
   taller. version counts the images given, 0 for none yet */
void ui_atlas_set(const u32* sprites, int w, int h);
const u32* ui_atlas(int* w, int* h, u32* version);
void ui_atlas_done(void);

/* the controls and the menu change on the main thread while the renderer draws them on its own: both under this */
void ui_lock(void);
void ui_unlock(void);
#endif
