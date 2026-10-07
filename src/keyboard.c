/* SPDX-License-Identifier: MIT */
/* The on-screen keyboard.
 *
 * Typing a WPA2 key on a d-pad is the worst text entry a handheld ever asks
 * for: long, mixed case, and full of symbols. Every decision here is about
 * cutting the number of button presses per character, because the naive
 * version - a grid with a shift KEY you navigate to - costs four to six moves
 * for every capital letter.
 *
 *   - The face buttons are verbs, not letters. B deletes, X shifts, Y goes to
 *     symbols. A capital costs one press instead of a round trip across the
 *     grid and back.
 *   - The digits row is permanent, on every layer. Passwords and usernames
 *     are full of digits, and putting them behind the symbols layer means a
 *     layer switch every few characters.
 *   - L1 and R1 move the cursor. Without them, fixing character 3 of a
 *     30-character key means 27 deletions and retyping the rest.
 *   - The grid wraps at every edge, which roughly halves worst-case travel.
 *
 * B is backspace rather than back, which is the one place this disagrees with
 * the rest of the UI. The reasoning is frequency: backspace is pressed dozens
 * of times per password and cancel once, so the cheap button belongs to the
 * common act. MENU cancels, and the hint bar says so.
 */
#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "platform.h"
#include "ui.h"

/* Four rows of ten, three layers. Digits are row 0 of every layer so they
 * never cost a switch. The tail of the letter rows carries the four symbols
 * that turn up most in passwords and mail addresses, so the common ones do
 * not need the symbol layer either. */
#define KB_COLS 10
#define KB_ROWS 4

static const char *LAYER[3] = {
	"1234567890"
	"abcdefghij"
	"klmnopqrst"
	"uvwxyz-_.@",

	"1234567890"
	"ABCDEFGHIJ"
	"KLMNOPQRST"
	"UVWXYZ-_.@",

	"1234567890"
	"!@#$%^&*()"
	"=+[]{}\\|/~"
	";:'\",.<>?`",
};

/* The space bar is its own row: one wide key under the grid. It is a key
 * rather than a face button because every face button is already a verb, and
 * a key you can see is better than one more thing to memorize. */
#define ROW_SPACE KB_ROWS

typedef struct {
	char buf[512];
	int  len, cur;      /* cursor sits BEFORE buf[cur] */
	int  layer;         /* 0 lower, 1 upper, 2 symbols */
	int  row, col;
} kb_state;

static void ins(kb_state *k, char c, int cap)
{
	if (k->len + 1 >= cap || k->len + 1 >= (int)sizeof k->buf) return;
	memmove(k->buf + k->cur + 1, k->buf + k->cur, (size_t)(k->len - k->cur) + 1);
	k->buf[k->cur] = c;
	k->cur++;
	k->len++;
}

static void del_left(kb_state *k)
{
	if (k->cur <= 0) return;
	memmove(k->buf + k->cur - 1, k->buf + k->cur, (size_t)(k->len - k->cur) + 1);
	k->cur--;
	k->len--;
}

/* ---------- drawing ------------------------------------------------------- */

#define PANEL_W   860
#define PANEL_H   580
#define KEY_W      72
#define KEY_H      60
#define KEY_GAP     6

static void draw_hint(SDL_Renderer *r, int x, int y, const char *btn,
                      const char *label)
{
	TTF_Font *fm = ui_font(UI_F_META);
	int bw = ui_text_width(fm, btn);
	/* The chip was sized to the LINE SKIP and the glyph drawn from the em
	 * box's top, so all of the unused descender depth landed below the text
	 * and every button sat high in its own box. These labels are A, B, X, Y,
	 * L/R, START, MENU - nothing in them descends at all, so the gap was pure
	 * and visible.
	 *
	 * Height from the em box, and the ink centered in it the way menu_draw
	 * centers its rows. Descent is negative, so half of it subtracted moves
	 * the line down onto the middle of the chip. */
	int fh = fm ? ui_font_box(fm) : ui_font_line(UI_F_META);
	int ink = fm ? -ui_font_descent(fm) / 2 : 0;
	int padv = 3;
	SDL_Rect chip = { x, y - padv, bw + 14, fh + padv * 2 };

	ui_round_rect(r, &chip, 6, (SDL_Color){ 52, 56, 70, 255 });
	ui_text(r, fm, btn, x + 7, y + ink, -1, UI_TEXT_SOFT);
	ui_text(r, fm, label, x + chip.w + 6, y + ink, -1, UI_TEXT_DIM);
}

static int hint_width(const char *btn, const char *label)
{
	TTF_Font *fm = ui_font(UI_F_META);
	return ui_text_width(fm, btn) + 14 + 6 + ui_text_width(fm, label) + 18;
}

static void kb_draw(SDL_Renderer *r, const kb_state *k, const char *title,
                    unsigned accent)
{
	SDL_Rect panel = { (TORTOS_SCREEN_W - PANEL_W) / 2,
	                   (TORTOS_SCREEN_H - PANEL_H) / 2, PANEL_W, PANEL_H };
	int gx = panel.x + (PANEL_W - (KB_COLS * KEY_W + (KB_COLS - 1) * KEY_GAP)) / 2;
	int gy = panel.y + 150;
	TTF_Font *fk = ui_font(UI_F_MENU);
	int i, c, fieldy;
	char shown[sizeof k->buf];

	ui_glow(r, &panel, accent, 60, 1.5f);
	ui_panel(r, &panel, 20, accent);

	ui_text(r, ui_font(UI_F_LABEL), title, panel.x + PANEL_W / 2,
	        panel.y + 22, 0, UI_TEXT_SOFT);

	/* The field. Always in clear - see the note in keyboard.h.
	 *
	 * Sized off the font, not a round number. It was 46 tall with the text
	 * laid in at +8, and a rendered line of UI_F_MENU is 44 at the default
	 * type scale - so the last six pixels were clipped away, which is exactly
	 * where g, j, p, q and y live. Reported from the device 2026-08-30 and
	 * plain in a shot: the descenders end in a flat horizontal cut.
	 *
	 * TTF_FontHeight is the height of the surface SDL_ttf renders, and it can
	 * come back a pixel or two over for some strings, so the padding is slack
	 * as well as margin. Five keeps the field clear of the key grid at the
	 * largest of the six text sizes, where the font is 64 tall. */
	fieldy = panel.y + 74;
	{
		int fh = fk ? ui_font_box(fk) : ui_font_line(UI_F_MENU);
		int fpad = 5;
		SDL_Rect f = { panel.x + 28, fieldy, PANEL_W - 56, fh + fpad * 2 };
		SDL_Rect clip = { f.x + 6, f.y, f.w - 12, f.h };
		int caret, pre, shift = 0;

		ui_round_rect(r, &f, 8, (SDL_Color){ 16, 18, 26, 255 });

		/* Width of everything left of the cursor, which is both where the
		 * caret goes and what the scroll is computed from. A WPA key runs to
		 * 63 characters and will not fit; without this the text simply walks
		 * out of the panel and the caret goes with it. */
		memcpy(shown, k->buf, (size_t)k->cur);
		shown[k->cur] = '\0';
		pre = ui_text_width(fk, shown);
		if (pre > clip.w - 16) shift = pre - (clip.w - 16);

		memcpy(shown, k->buf, (size_t)k->len);
		shown[k->len] = '\0';
		SDL_RenderSetClipRect(r, &clip);
		ui_text(r, fk, shown, f.x + 12 - shift, f.y + fpad, -1, UI_TEXT);
		SDL_RenderSetClipRect(r, NULL);

		/* Caret at the measured pixel width rather than a fixed advance,
		 * which a proportional face would not honor. Its height follows the
		 * font for the same reason the field does - it was 32 pixels, which
		 * is a caret shorter than the letters at every size but the smallest.
		 * A pixel in from the text's own box, so it reads as a cursor rather
		 * than a rule. */
		caret = f.x + 12 + pre - shift;
		SDL_SetRenderDrawColor(r, (Uint8)(accent >> 16), (Uint8)(accent >> 8),
		                       (Uint8)accent, 255);
		SDL_RenderFillRect(r, &(SDL_Rect){ caret, f.y + fpad + 1, 2, fh - 2 });
	}

	/* The grid. */
	for (i = 0; i < KB_ROWS; i++) {
		for (c = 0; c < KB_COLS; c++) {
			char lbl[2] = { LAYER[k->layer][i * KB_COLS + c], '\0' };
			SDL_Rect q = { gx + c * (KEY_W + KEY_GAP), gy + i * (KEY_H + KEY_GAP),
			               KEY_W, KEY_H };
			bool sel = (k->row == i && k->col == c);
			ui_round_rect(r, &q, 8, sel ? (SDL_Color){ 62, 68, 86, 255 }
			                           : (SDL_Color){ 32, 35, 46, 255 });
			ui_text(r, fk, lbl, q.x + KEY_W / 2, q.y + 12, 0,
			        sel ? UI_TEXT : UI_TEXT_SOFT);
		}
	}

	/* The space bar. */
	{
		SDL_Rect q = { gx, gy + KB_ROWS * (KEY_H + KEY_GAP),
		               KB_COLS * KEY_W + (KB_COLS - 1) * KEY_GAP, KEY_H - 12 };
		bool sel = (k->row == ROW_SPACE);
		ui_round_rect(r, &q, 8, sel ? (SDL_Color){ 62, 68, 86, 255 }
		                           : (SDL_Color){ 32, 35, 46, 255 });
		ui_text(r, ui_font(UI_F_META), "space", q.x + q.w / 2, q.y + 8, 0,
		        sel ? UI_TEXT : UI_TEXT_DIM);
	}

	/* The hint bar. Every verb has a button; showing the glyph is what
	 * teaches the shortcut, and is why none of them costs a grid cell. */
	{
		static const char *btn[] = { "A", "B", "X", "Y", "L/R", "START", "MENU" };
		const char *lbl[7];
		int fl = ui_font_line(UI_F_META);
		int lh = fl + 8;
		/* Positioned from the panel's bottom edge, not from a guess. The
		 * first attempt hung the second row past the border and it picked up
		 * the shelf tint through it, which reads as a rendering fault. */
		int y0 = panel.y + PANEL_H - 20 - fl - lh;
		int split = 4, total, hx, n;

		lbl[0] = "type";  lbl[1] = "delete";
		lbl[2] = k->layer == 1 ? "unshift" : "shift";
		lbl[3] = k->layer == 2 ? "letters" : "symbols";
		lbl[4] = "cursor"; lbl[5] = "done";   lbl[6] = "cancel";

		/* Two rows, and measured rather than assumed. Seven of these on one
		 * line came to more than the panel is wide, and the ends were sliced
		 * off against the border - which is the sort of thing that looks
		 * deliberate in a screenshot and is not. */
		for (n = 0; n < 2; n++) {
			int from = n ? split : 0, to = n ? 7 : split;
			total = 0;
			for (i = from; i < to; i++) total += hint_width(btn[i], lbl[i]);
			hx = panel.x + (PANEL_W - total) / 2;
			for (i = from; i < to; i++) {
				draw_hint(r, hx, y0 + n * lh, btn[i], lbl[i]);
				hx += hint_width(btn[i], lbl[i]);
			}
		}
	}
}

void kb_preview(SDL_Renderer *r, const char *title, const char *text,
                int layer, int row, int col, unsigned accent)
{
	kb_state k;

	memset(&k, 0, sizeof k);
	snprintf(k.buf, sizeof k.buf, "%s", text ? text : "");
	k.len = (int)strlen(k.buf);
	k.cur = k.len;
	k.layer = layer;
	k.row = row;
	k.col = col;
	kb_draw(r, &k, title, accent);
}

/* ---------- the loop ------------------------------------------------------ */

kb_result kb_prompt(SDL_Renderer *r, in_state *in, const char *title,
                    char *buf, int cap, unsigned accent,
                    kb_backdrop backdrop, kb_power power, void *ctx)
{
	kb_state k;
	int cols;

	memset(&k, 0, sizeof k);
	snprintf(k.buf, sizeof k.buf, "%s", buf ? buf : "");
	k.len = (int)strlen(k.buf);
	k.cur = k.len;
	k.row = 1;              /* open on the letters, not the digits */

	ui_pace pace = {0};

	for (;;) {
		plat_input_poll(in);
		if (in->quit_requested) return KB_CANCEL;
		/* Typing a password is the longest anyone stares at this device
		 * without pressing anything, and it was the one screen where that
		 * could go on forever. */
		if (power ? power(ctx) : in->pressed[IN_POWER]) return KB_POWER;

		cols = (k.row == ROW_SPACE) ? 1 : KB_COLS;

		/* Wrapping in both axes: the grid is a torus, which is what keeps the
		 * worst case from being a trip across ten columns. */
		if (in_repeat(in, IN_LEFT))  k.col = (k.col + cols - 1) % cols;
		if (in_repeat(in, IN_RIGHT)) k.col = (k.col + 1) % cols;
		if (in_repeat(in, IN_UP))    k.row = (k.row + ROW_SPACE) % (ROW_SPACE + 1);
		if (in_repeat(in, IN_DOWN))  k.row = (k.row + 1) % (ROW_SPACE + 1);
		if (k.row == ROW_SPACE) k.col = 0;

		/* Cursor, on the shoulders. Repeating, because walking a long key one
		 * press at a time is the thing this exists to avoid. */
		if (in_repeat(in, IN_L1) && k.cur > 0)     k.cur--;
		if (in_repeat(in, IN_R1) && k.cur < k.len) k.cur++;

		if (in->pressed[IN_ACCEPT]) {
			if (k.row == ROW_SPACE) ins(&k, ' ', cap);
			else ins(&k, LAYER[k.layer][k.row * KB_COLS + k.col], cap);
		}
		/* Repeating, so holding it clears a field instead of asking for
		 * thirty presses. */
		if (in_repeat(in, IN_BACK)) del_left(&k);

		/* Sticky, not held: you cannot comfortably hold a face button and
		 * work the d-pad at the same time on this shell. */
		if (in->pressed[IN_X]) k.layer = (k.layer == 1) ? 0 : 1;
		if (in->pressed[IN_Y]) k.layer = (k.layer == 2) ? 0 : 2;

		if (in->pressed[IN_START]) {
			snprintf(buf, (size_t)cap, "%s", k.buf);
			return KB_ACCEPT;
		}
		if (in->pressed[IN_MENU]) return KB_CANCEL;

		/* Volume and brightness keep working here, as they do everywhere. */
		if (in_repeat(in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* Drawn only when something changed (ui_draw_due): nothing on a
		 * keyboard moves by itself, so a press or a hold is a frame. */
		{
			bool touched = false;
			int i;

			for (i = 0; i < IN_COUNT && !touched; i++)
				touched = in->pressed[i] || in->down[i];
			if (!ui_draw_due(&pace, touched, 0)) {
				SDL_Delay(UI_IDLE_POLL_MS);
				continue;
			}
		}
		if (backdrop) backdrop(ctx);
		kb_draw(r, &k, title, accent);
		plat_draw_osd(r);
		plat_present(r);
	}
}
