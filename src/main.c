#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include "html.h"
#include "net.h"
#include "github.h"

#define SW 960
#define SH 544
#define BAR 48
#define MARGIN 24
#define HMAX 32

#define C_BG     RGBA8(18, 18, 22, 255)
#define C_BAR    RGBA8(30, 30, 36, 255)
#define C_PILL   RGBA8(48, 48, 58, 255)
#define C_TEXT   RGBA8(228, 228, 234, 255)
#define C_DIM    RGBA8(140, 140, 152, 255)
#define C_LINK   RGBA8(112, 172, 255, 255)
#define C_CODE   RGBA8(170, 220, 170, 255)
#define C_ACCENT RGBA8(90, 140, 255, 255)
#define C_TILE   RGBA8(36, 36, 44, 255)
#define C_SHADE  RGBA8(0, 0, 0, 150)

/* gesture tuning */
#define EDGE_W        28     /* edge-swipe start zone, pixels */
#define EDGE_TRIGGER  110    /* swipe distance for back/forward */
#define PULL_TRIGGER  70     /* pull distance for refresh */
#define LONG_PRESS    30     /* frames (~0.5 s) */
#define ZOOM_MIN      0.6f
#define ZOOM_MAX      2.5f

typedef struct {
  Page page;
  char cur[1024];            /* "" = home screen */
  char hist[HMAX][1024];
  int hn, hp;
  float scroll, vel;
  int ok;                    /* last load succeeded */
  float zoom;                /* zoom the page was laid out at */
} Tab;

#define MAXTABS 9
#define MAXBM 64
#define BM_ROW 56

static Tab tabs[MAXTABS];
static int ntabs = 1, act = 0;
#define T tabs[act]

typedef struct { char title[96]; char url[512]; } Bookmark;
static Bookmark bm[MAXBM];
static int nbm = 0;

enum { MODE_PAGE, MODE_TABS, MODE_BOOKMARKS };

static struct {
  vita2d_pgf *pgf;
  int loading;
  int mode;
  float bscroll;
  char toast[200];
  int toast_t;
  float zoom;
  /* gestures */
  int press_link;            /* link under the finger (for highlight), -1 none */
  int menu_link;             /* long-press menu open for this link, -1 none */
  int edge;                  /* -1 left edge (back), 1 right edge (forward), 0 none */
  float edge_dx;
  float pull;                /* pull-to-refresh distance */
  /* download in progress */
  int dl_active;
  char dl_name[128];
  double dl_done, dl_total;
  unsigned old_buttons;      /* buttons held last frame */
} g;

/* After a dialog or download, treat buttons that are still held as old,
   so the press that closed it doesn't also act on the page. */
static void swallow_buttons(void) {
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(0, &pad, 1);
  g.old_buttons = pad.buttons;
}

#define NTILES 6
static const struct { const char *name; const char *url; } tiles[NTILES] = {
  {"Wikipedia",   "https://en.m.wikipedia.org"},
  {"DuckDuckGo",  "https://lite.duckduckgo.com/lite/"},
  {"GitHub",      "gh:"},
  {"Hacker News", "https://news.ycombinator.com"},
  {"NPR Text",    "https://text.npr.org"},
  {"Lobsters",    "https://lobste.rs"},
};
#define TILE_W 260
#define TILE_H 110
static int tile_x(int i) { return 75 + (i % 3) * (TILE_W + 20); }
static int tile_y(int i) { return 196 + (i / 3) * (TILE_H + 20); }

/* ---------- helpers ---------- */

static int text_w(const char *t, float sc) { return vita2d_pgf_text_width(g.pgf, sc, t); }
static int measure(const char *t, int style) { return text_w(t, html_style_scale[style] * g.zoom); }

static void text_at(float x, float y, unsigned col, float sc, const char *t) {
  vita2d_pgf_draw_text(g.pgf, x, y, col, sc, t);
}

static void fit_text(char *dst, size_t dsz, const char *src, float sc, int maxw) {
  snprintf(dst, dsz, "%s", src);
  if (text_w(dst, sc) <= maxw) return;
  size_t n = strlen(dst);
  while (n > 3 && text_w(dst, sc) > maxw) { n--; dst[n] = 0; if (n > 3) { dst[n-1] = '.'; dst[n-2] = '.'; dst[n-3] = '.'; } }
}

static int content_h(void) { return SH - BAR; }
static float max_scroll(void) {
  float m = T.page.height + 40 - content_h();
  return m > 0 ? m : 0;
}

static const char *host_of(const char *url) {
  if (gh_is(url)) return "GitHub";
  const char *p = strstr(url, "://");
  return p ? p + 3 : url;
}

static void toast(const char *msg) { snprintf(g.toast, sizeof(g.toast), "%s", msg); g.toast_t = 150; }

static void relayout(void) {
  html_layout(&T.page, measure, SW - 2 * MARGIN, g.zoom);
  T.zoom = g.zoom;
}

/* ---------- settings (zoom) ---------- */

#define DATA_DIR "ux0:data/VitaSurf"
#define SETTINGS DATA_DIR "/settings.txt"

static void ensure_dir(void) { sceIoMkdir("ux0:data", 0777); sceIoMkdir(DATA_DIR, 0777); }

static void settings_load(void) {
  g.zoom = 1.0f;
  FILE *f = fopen(SETTINGS, "r");
  if (!f) return;
  float z;
  if (fscanf(f, "zoom=%f", &z) == 1 && z >= ZOOM_MIN && z <= ZOOM_MAX) g.zoom = z;
  fclose(f);
}

static void settings_save(void) {
  ensure_dir();
  FILE *f = fopen(SETTINGS, "w");
  if (f) { fprintf(f, "zoom=%.2f\n", g.zoom); fclose(f); }
}

/* ---------- bookmarks ---------- */

#define BM_FILE DATA_DIR "/bookmarks.txt"

static void bm_load(void) {
  FILE *f = fopen(BM_FILE, "r");
  if (!f) return;
  char line[700];
  while (nbm < MAXBM && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    snprintf(bm[nbm].url, sizeof(bm[nbm].url), "%.511s", line);
    snprintf(bm[nbm].title, sizeof(bm[nbm].title), "%.95s", tab + 1);
    nbm++;
  }
  fclose(f);
}

static void bm_save(void) {
  ensure_dir();
  FILE *f = fopen(BM_FILE, "w");
  if (!f) return;
  for (int i = 0; i < nbm; i++) fprintf(f, "%s\t%s\n", bm[i].url, bm[i].title);
  fclose(f);
}

static int bm_find(const char *url) {
  for (int i = 0; i < nbm; i++) if (strcmp(bm[i].url, url) == 0) return i;
  return -1;
}

static void bm_add(const char *url, const char *title) {
  if (bm_find(url) >= 0) { toast("Already bookmarked"); return; }
  if (nbm >= MAXBM) { toast("Bookmark list is full"); return; }
  snprintf(bm[nbm].url, sizeof(bm[nbm].url), "%.511s", url);
  snprintf(bm[nbm].title, sizeof(bm[nbm].title), "%.95s", title[0] ? title : host_of(url));
  for (char *c = bm[nbm].title; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
  nbm++;
  bm_save();
  toast("Bookmark added");
}

static void bm_delete(int i) {
  if (i < 0 || i >= nbm) return;
  memmove(&bm[i], &bm[i + 1], (nbm - i - 1) * sizeof(Bookmark));
  nbm--;
  bm_save();
}

static void bm_toggle_current(void) {
  if (!T.cur[0]) return;
  int i = bm_find(T.cur);
  if (i >= 0) { bm_delete(i); toast("Bookmark removed"); }
  else bm_add(T.cur, T.page.title[0] ? T.page.title : host_of(T.cur));
}

static float bm_max_scroll(void) {
  float m = nbm * BM_ROW + 20 - (SH - BAR);
  return m > 0 ? m : 0;
}

/* ---------- tabs ---------- */

static int new_tab(void) {
  if (ntabs >= MAXTABS) { toast("Tab limit reached"); return 0; }
  memset(&tabs[ntabs], 0, sizeof(Tab));
  act = ntabs++;
  return 1;
}

static void close_tab(int i) {
  html_free(&tabs[i].page);
  if (ntabs == 1) { memset(&tabs[0], 0, sizeof(Tab)); return; }
  memmove(&tabs[i], &tabs[i + 1], (ntabs - i - 1) * sizeof(Tab));
  ntabs--;
  memset(&tabs[ntabs], 0, sizeof(Tab));
  if (i < act) act--;
  if (act >= ntabs) act = ntabs - 1;
}

#define CARD_W 280
#define CARD_H 110
static int card_x(int i) { return 40 + (i % 3) * (CARD_W + 20); }
static int card_y(int i) { return BAR + 24 + (i / 3) * (CARD_H + 20); }

/* ---------- link helpers ---------- */

static int link_at(int x, int y) {
  if (!T.cur[0]) return -1;
  int cx = x - MARGIN;
  float cy = y - BAR - MARGIN / 2 + T.scroll;
  for (int i = 0; i < T.page.nwords; i++) {
    Word *w = &T.page.words[i];
    if (w->link < 0) continue;
    if (cx >= w->x - 4 && cx <= w->x + w->w + 4 && cy >= w->y - 4 && cy <= w->y + w->h + 4) return w->link;
  }
  return -1;
}

static void link_text(int link, char *out, size_t osz) {
  out[0] = 0;
  size_t n = 0;
  for (int i = 0; i < T.page.nwords && n + 2 < osz; i++) {
    if (T.page.words[i].link != link) continue;
    n += snprintf(out + n, osz - n, "%s%s", n ? " " : "", T.page.words[i].text);
  }
  if (n >= osz) out[osz - 1] = 0;
}

/* ---------- drawing ---------- */

static void draw_home(void) {
  const char *title = "VitaSurf";
  text_at((SW - text_w(title, 2.0f)) / 2, BAR + 90, C_TEXT, 2.0f, title);
  const char *sub = "Tap a tile, or press Triangle to search or type an address";
  text_at((SW - text_w(sub, 1.0f)) / 2, BAR + 125, C_DIM, 1.0f, sub);
  for (int i = 0; i < NTILES; i++) {
    int x = tile_x(i), y = tile_y(i);
    vita2d_draw_rectangle(x, y, TILE_W, TILE_H, C_TILE);
    vita2d_draw_rectangle(x, y, 6, TILE_H, C_ACCENT);
    text_at(x + 26, y + 52, C_TEXT, 1.25f, tiles[i].name);
    text_at(x + 26, y + 84, C_DIM, 0.8f, gh_is(tiles[i].url) ? "repos, files, releases" : host_of(tiles[i].url));
  }
  text_at(SW - 24 - text_w("build " BUILD_ID, 0.75f), SH - 12, C_DIM, 0.75f, "build " BUILD_ID);
}

static void draw_page(void) {
  if (T.zoom != g.zoom) relayout();
  float top = BAR + MARGIN / 2 + g.pull;
  vita2d_set_clip_rectangle(0, BAR, SW, SH);
  vita2d_enable_clipping();
  for (int i = 0; i < T.page.nwords; i++) {
    Word *w = &T.page.words[i];
    float sy = top + w->y - T.scroll;
    if (sy + w->h < BAR || sy > SH) continue;
    float sc = html_style_scale[w->style] * T.zoom;
    unsigned col = w->link >= 0 ? C_LINK : w->style == 3 ? C_DIM : w->style == 5 ? C_CODE : C_TEXT;
    if (w->link >= 0 && w->link == g.press_link)
      vita2d_draw_rectangle(MARGIN + w->x - 3, sy, w->w + 6, w->h, RGBA8(90, 140, 255, 70));
    text_at(MARGIN + w->x, sy + w->h * 0.78f, col, sc, w->text);
    if (w->link >= 0) vita2d_draw_rectangle(MARGIN + w->x, sy + w->h * 0.88f, w->w, 1, col);
  }
  vita2d_disable_clipping();

  float ms = max_scroll();
  if (ms > 0) {
    float vh = content_h();
    float th = vh * vh / (T.page.height + 40);
    if (th < 30) th = 30;
    float ty = BAR + (vh - th) * (T.scroll / ms);
    vita2d_draw_rectangle(SW - 6, ty, 4, th, RGBA8(255, 255, 255, 70));
  }
}

static void draw_tabs(void) {
  vita2d_draw_rectangle(0, BAR, SW, SH - BAR, C_BG);
  int count = ntabs + (ntabs < MAXTABS ? 1 : 0);
  for (int i = 0; i < count; i++) {
    int x = card_x(i), y = card_y(i);
    if (i == ntabs) {
      vita2d_draw_rectangle(x, y, CARD_W, CARD_H, C_PILL);
      const char *l = "+  New tab";
      text_at(x + (CARD_W - text_w(l, 1.2f)) / 2, y + CARD_H / 2 + 8, C_TEXT, 1.2f, l);
      continue;
    }
    if (i == act) vita2d_draw_rectangle(x - 3, y - 3, CARD_W + 6, CARD_H + 6, C_ACCENT);
    vita2d_draw_rectangle(x, y, CARD_W, CARD_H, C_TILE);
    const char *title = tabs[i].cur[0]
        ? (tabs[i].page.title[0] ? tabs[i].page.title : host_of(tabs[i].cur)) : "Home";
    char buf[128];
    fit_text(buf, sizeof(buf), title, 1.0f, CARD_W - 64);
    text_at(x + 16, y + 40, C_TEXT, 1.0f, buf);
    fit_text(buf, sizeof(buf), tabs[i].cur[0] ? host_of(tabs[i].cur) : "New tab", 0.85f, CARD_W - 32);
    text_at(x + 16, y + 76, C_DIM, 0.85f, buf);
    text_at(x + CARD_W - 32, y + 34, C_DIM, 1.2f, "x");
  }
}

static void draw_bookmarks(void) {
  vita2d_draw_rectangle(0, BAR, SW, SH - BAR, C_BG);
  if (nbm == 0) {
    const char *m = "No bookmarks yet. Tap the star, or hold a link, to save one.";
    text_at((SW - text_w(m, 1.0f)) / 2, BAR + 200, C_DIM, 1.0f, m);
    return;
  }
  vita2d_set_clip_rectangle(0, BAR, SW, SH);
  vita2d_enable_clipping();
  for (int i = 0; i < nbm; i++) {
    float y = BAR + 10 + i * BM_ROW - g.bscroll;
    if (y + BM_ROW < BAR || y > SH) continue;
    vita2d_draw_rectangle(24, y, SW - 48, BM_ROW - 8, C_TILE);
    char buf[160];
    fit_text(buf, sizeof(buf), bm[i].title, 1.0f, SW - 180);
    text_at(44, y + 24, C_TEXT, 1.0f, buf);
    fit_text(buf, sizeof(buf), bm[i].url, 0.8f, SW - 180);
    text_at(44, y + 44, C_DIM, 0.8f, buf);
    text_at(SW - 70, y + 32, C_DIM, 1.2f, "x");
  }
  vita2d_disable_clipping();
}

/* long-press menu */
#define MENU_W 440
#define MENU_ROW 56
#define MENU_N 4
static const char *menu_items[MENU_N] = {"Open", "Open in new tab", "Bookmark link", "Show address"};
static int menu_x(void) { return (SW - MENU_W) / 2; }
static int menu_y(void) { return (SH - (MENU_N * MENU_ROW + 60)) / 2 + 10; }

static void draw_menu(void) {
  vita2d_draw_rectangle(0, 0, SW, SH, C_SHADE);
  int x = menu_x(), y = menu_y();
  vita2d_draw_rectangle(x, y, MENU_W, MENU_N * MENU_ROW + 60, C_BAR);
  char buf[200];
  fit_text(buf, sizeof(buf), T.page.links[g.menu_link], 0.85f, MENU_W - 40);
  text_at(x + 20, y + 36, C_DIM, 0.85f, buf);
  for (int i = 0; i < MENU_N; i++) {
    int ry = y + 60 + i * MENU_ROW;
    vita2d_draw_rectangle(x + 12, ry, MENU_W - 24, MENU_ROW - 8, C_PILL);
    const char *label = (i == 0 && gh_is_download(T.page.links[g.menu_link])) ? "Download" : menu_items[i];
    text_at(x + 32, ry + 32, C_TEXT, 1.0f, label);
  }
}

static void draw_gestures(void) {
  /* edge swipe arrow */
  if (g.edge) {
    float d = fminf(fabsf(g.edge_dx), 160);
    int ready = d >= EDGE_TRIGGER;
    float cx = g.edge < 0 ? -30 + d * 0.6f : SW + 30 - d * 0.6f;
    vita2d_draw_fill_circle(cx, SH / 2, 30, ready ? C_ACCENT : C_PILL);
    const char *a = g.edge < 0 ? "<" : ">";
    text_at(cx - text_w(a, 1.4f) / 2, SH / 2 + 10, C_TEXT, 1.4f, a);
  }
  /* pull to refresh */
  if (g.pull > 4) {
    int ready = g.pull >= PULL_TRIGGER;
    float cy = BAR + fminf(g.pull, 110) - 24;
    vita2d_draw_fill_circle(SW / 2, cy, 20, ready ? C_ACCENT : C_PILL);
    text_at(SW / 2 - text_w("R", 1.0f) / 2, cy + 7, C_TEXT, 1.0f, "R");
    const char *l = ready ? "Release to refresh" : "Pull to refresh";
    text_at(SW / 2 + 32, cy + 7, C_DIM, 0.85f, l);
  }
}

static void draw_download(void) {
  vita2d_draw_rectangle(0, 0, SW, SH, C_SHADE);
  int w = 560, h = 150, x = (SW - w) / 2, y = (SH - h) / 2;
  vita2d_draw_rectangle(x, y, w, h, C_BAR);
  char buf[200];
  fit_text(buf, sizeof(buf), g.dl_name, 1.0f, w - 40);
  text_at(x + 20, y + 40, C_TEXT, 1.0f, buf);
  float frac = g.dl_total > 0 ? (float)(g.dl_done / g.dl_total) : 0;
  vita2d_draw_rectangle(x + 20, y + 62, w - 40, 14, C_PILL);
  vita2d_draw_rectangle(x + 20, y + 62, (w - 40) * frac, 14, C_ACCENT);
  if (g.dl_total > 0)
    snprintf(buf, sizeof(buf), "%.1f of %.1f MB  (%d%%)", g.dl_done / 1048576, g.dl_total / 1048576, (int)(frac * 100));
  else snprintf(buf, sizeof(buf), "%.1f MB", g.dl_done / 1048576);
  text_at(x + 20, y + 104, C_DIM, 0.9f, buf);
  text_at(x + 20, y + 132, C_DIM, 0.8f, "Press Circle to cancel");
}

static void draw_btn(int cx, const char *label, unsigned col) {
  vita2d_draw_fill_circle(cx, BAR / 2, 18, C_PILL);
  text_at(cx - text_w(label, 1.0f) / 2, BAR / 2 + 7, col, 1.0f, label);
}

static void render(void) {
  vita2d_start_drawing();
  vita2d_clear_screen();

  if (g.mode == MODE_TABS) draw_tabs();
  else if (g.mode == MODE_BOOKMARKS) draw_bookmarks();
  else if (T.cur[0]) draw_page();
  else draw_home();

  vita2d_draw_rectangle(0, 0, SW, BAR, C_BAR);
  draw_btn(30, "<", C_TEXT); draw_btn(78, ">", C_TEXT);
  draw_btn(126, "R", C_TEXT); draw_btn(174, "H", C_TEXT);

  int px0 = 214, px1 = SW - 156;
  vita2d_draw_fill_circle(px0, BAR / 2, 16, C_PILL);
  vita2d_draw_fill_circle(px1, BAR / 2, 16, C_PILL);
  vita2d_draw_rectangle(px0, 8, px1 - px0, 32, C_PILL);

  char buf[256];
  const char *label;
  unsigned col = C_TEXT;
  if (g.loading) label = "Loading...";
  else if (!T.cur[0]) { label = "Search or enter address"; col = C_DIM; }
  else label = T.page.title[0] ? T.page.title : host_of(T.cur);
  fit_text(buf, sizeof(buf), label, 1.0f, px1 - px0 - 24);
  text_at(px0 + 16, BAR / 2 + 7, col, 1.0f, buf);

  draw_btn(SW - 120, "*", (T.cur[0] && bm_find(T.cur) >= 0) ? RGBA8(255, 200, 60, 255) : C_DIM);
  draw_btn(SW - 72, "=", g.mode == MODE_BOOKMARKS ? C_ACCENT : C_TEXT);
  vita2d_draw_fill_circle(SW - 24, BAR / 2, 18, g.mode == MODE_TABS ? C_ACCENT : C_PILL);
  char n[4];
  snprintf(n, sizeof(n), "%d", ntabs);
  text_at(SW - 24 - text_w(n, 1.0f) / 2, BAR / 2 + 7, C_TEXT, 1.0f, n);

  int secure = T.ok && (strncmp(T.cur, "https://", 8) == 0 || gh_is(T.cur));
  if (g.mode == MODE_PAGE && T.cur[0] && secure && !g.loading)
    vita2d_draw_rectangle(0, BAR - 2, SW, 2, RGBA8(70, 190, 120, 255));
  if (g.loading) vita2d_draw_rectangle(0, BAR - 2, SW, 2, C_ACCENT);

  if (g.mode == MODE_PAGE) draw_gestures();
  if (g.menu_link >= 0) draw_menu();
  if (g.dl_active) draw_download();

  if (g.toast_t > 0) {
    g.toast_t--;
    char tb[200];
    fit_text(tb, sizeof(tb), g.toast, 1.0f, SW - 80);
    int tw = text_w(tb, 1.0f);
    vita2d_draw_rectangle((SW - tw) / 2 - 20, SH - 70, tw + 40, 40, RGBA8(60, 60, 72, 240));
    text_at((SW - tw) / 2, SH - 43, C_TEXT, 1.0f, tb);
  }

  vita2d_end_drawing();
  vita2d_common_dialog_update();
  vita2d_swap_buffers();
}

/* ---------- keyboard ---------- */

static int ime_input(const char *title_text, const char *initial, char *out, size_t osz) {
  static SceWChar16 title[64], init[512], input[512 + 1];
  int i;
  for (i = 0; title_text[i] && i < 63; i++) title[i] = (unsigned char)title_text[i];
  title[i] = 0;
  for (i = 0; initial[i] && i < 510; i++) init[i] = (unsigned char)initial[i];
  init[i] = 0;
  memset(input, 0, sizeof(input));

  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH_GB;
  p.languagesForced = SCE_FALSE;
  p.type = SCE_IME_TYPE_BASIC_LATIN;
  p.option = 0;
  p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  p.maxTextLength = 512;
  p.title = title;
  p.initialText = init;
  p.inputTextBuffer = input;
  if (sceImeDialogInit(&p) < 0) return 0;

  while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) render();

  SceImeDialogResult r;
  memset(&r, 0, sizeof(r));
  sceImeDialogGetResult(&r);
  int ok = (r.button == SCE_IME_DIALOG_BUTTON_ENTER);
  if (ok) {
    size_t o = 0;
    for (i = 0; input[i] && o + 1 < osz; i++) if (input[i] < 128) out[o++] = (char)input[i];
    out[o] = 0;
  }
  sceImeDialogTerm();
  swallow_buttons();
  return ok && out[0];
}

static void trim(char *s) {
  char *a = s;
  while (*a == ' ') a++;
  memmove(s, a, strlen(a) + 1);
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
}

static void url_encode(const char *s, char *out, size_t osz) {
  size_t o = 0;
  for (; *s && o + 4 < osz; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.')
      out[o++] = c;
    else if (c == ' ') out[o++] = '+';
    else o += snprintf(out + o, osz - o, "%%%02X", c);
  }
  out[o] = 0;
}

/* ---------- navigation ---------- */

static int dl_progress(void *user, double done, double total) {
  (void)user;
  g.dl_done = done;
  g.dl_total = total;
  render();
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(0, &pad, 1);
  return (pad.buttons & SCE_CTRL_CIRCLE) ? 1 : 0;
}

static void start_download(const char *url) {
  char saved[400], err[400];
  const char *name = strrchr(url, '|');
  snprintf(g.dl_name, sizeof(g.dl_name), "Downloading %s", name ? name + 1 : "file");
  g.dl_active = 1;
  g.dl_done = g.dl_total = 0;
  render();
  int rc = gh_download(url, dl_progress, NULL, saved, sizeof(saved), err, sizeof(err));
  g.dl_active = 0;
  swallow_buttons();
  char msg[500];
  if (rc == 0) snprintf(msg, sizeof(msg), "Saved to %s", saved);
  else snprintf(msg, sizeof(msg), "Download failed: %s", err);
  toast(msg);
}

static void navigate(const char *url, int push);

/* App actions behind special gh: links. Returns 1 if handled. */
static int gh_action(const char *url) {
  char in[600], target[900];
  if (!strcmp(url, "gh:signin")) {
    if (!ime_input("Paste a GitHub personal access token", "", in, sizeof(in))) return 1;
    trim(in);
    gh_set_token(in);
    toast("Signed in to GitHub");
    navigate("gh:", 1);
    return 1;
  }
  if (!strcmp(url, "gh:signout")) {
    gh_set_token("");
    toast("Signed out of GitHub");
    navigate("gh:", 1);
    return 1;
  }
  if (!strcmp(url, "gh:searchbox")) {
    if (!ime_input("Search GitHub repositories", "", in, sizeof(in))) return 1;
    trim(in);
    char q[700];
    url_encode(in, q, sizeof(q));
    snprintf(target, sizeof(target), "gh:search?q=%s", q);
    navigate(target, 1);
    return 1;
  }
  if (!strcmp(url, "gh:openbox")) {
    if (!ime_input("User name, or owner/repository", "", in, sizeof(in))) return 1;
    trim(in);
    if (strchr(in, '/')) snprintf(target, sizeof(target), "gh:repo/%s", in);
    else snprintf(target, sizeof(target), "gh:user/%s", in);
    navigate(target, 1);
    return 1;
  }
  return 0;
}

static void navigate(const char *url, int push) {
  char target[1024];
  snprintf(target, sizeof(target), "%s", url);
  g.menu_link = -1;
  g.press_link = -1;

  /* github.com pages need JavaScript; show them in GitHub mode instead */
  char conv[1024];
  if (gh_from_web(target, conv, sizeof(conv))) snprintf(target, sizeof(target), "%s", conv);
  if (gh_is_download(target)) { start_download(target); return; }
  if (gh_is(target) && gh_action(target)) return;

  g.loading = 1;
  render();

  html_free(&T.page);
  if (gh_is(target)) {
    char base[1024];
    char *html = gh_render(target, base, sizeof(base));
    snprintf(T.cur, sizeof(T.cur), "%s", target);
    T.ok = 1;
    html_parse(&T.page, html ? html : "<p>Out of memory</p>", base);
    free(html);
  } else {
    char *buf = NULL; size_t len = 0;
    char fin[1024], err[400];
    if (net_fetch(target, &buf, &len, fin, sizeof(fin), err, sizeof(err)) == 0) {
      snprintf(T.cur, sizeof(T.cur), "%s", fin);
      T.ok = 1;
      html_parse(&T.page, buf, fin);
      free(buf);
    } else {
      snprintf(T.cur, sizeof(T.cur), "%s", target);
      T.ok = 0;
      char msg[1600];
      snprintf(msg, sizeof(msg), "<h2>Could not load page</h2><p>%.600s</p><p>%s</p>", target, err);
      html_parse(&T.page, msg, target);
    }
  }
  relayout();
  T.scroll = 0; T.vel = 0;

  if (push) {
    if (T.hn > 0) T.hn = T.hp + 1;
    if (T.hn == HMAX) { memmove(T.hist[0], T.hist[1], (HMAX - 1) * 1024); T.hn--; }
    memcpy(T.hist[T.hn], T.cur, sizeof(T.cur));
    T.hp = T.hn++;
  }
  g.loading = 0;
}

static void go_home(void) { html_free(&T.page); T.cur[0] = 0; T.scroll = T.vel = 0; }
static void go_back(void) {
  if (!T.cur[0]) { if (T.hn) navigate(T.hist[T.hp], 0); }
  else if (T.hp > 0) { T.hp--; navigate(T.hist[T.hp], 0); }
}
static void go_fwd(void) { if (T.hp + 1 < T.hn) { T.hp++; navigate(T.hist[T.hp], 0); } }
static void reload(void) { if (T.cur[0]) { char u[1024]; snprintf(u, sizeof(u), "%s", T.cur); navigate(u, 0); } }

static void open_address_bar(void) {
  char in[600];
  if (!ime_input("Search or enter address", T.cur, in, sizeof(in))) return;
  trim(in);
  if (!in[0]) return;

  char url[1100];
  if (!strncmp(in, "gh ", 3)) {                      /* "gh owner/repo" or "gh user" */
    char *a = in + 3;
    while (*a == ' ') a++;
    snprintf(url, sizeof(url), strchr(a, '/') ? "gh:repo/%s" : "gh:user/%s", a);
  }
  else if (gh_is(in) || strstr(in, "://")) snprintf(url, sizeof(url), "%s", in);
  else if (!strchr(in, ' ') && strchr(in, '.')) snprintf(url, sizeof(url), "https://%s", in);
  else {
    char q[700];
    url_encode(in, q, sizeof(q));
    snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", q);
  }
  navigate(url, 1);
}

/* ---------- taps ---------- */

static void open_link(int link, int in_new_tab) {
  char u[1024];
  snprintf(u, sizeof(u), "%s", T.page.links[link]);
  if (in_new_tab && !gh_is_download(u)) {
    if (!new_tab()) return;
    toast("Opened in a new tab");
  }
  navigate(u, 1);
}

static void menu_tap(int x, int y) {
  int link = g.menu_link;
  g.menu_link = -1;
  int mx = menu_x(), my = menu_y();
  if (x < mx || x >= mx + MENU_W) return;
  int i = (y - my - 60) / MENU_ROW;
  if (y < my + 60 || i < 0 || i >= MENU_N) return;
  char text[96];
  switch (i) {
    case 0: open_link(link, 0); break;
    case 1: open_link(link, 1); break;
    case 2: link_text(link, text, sizeof(text)); bm_add(T.page.links[link], text); break;
    case 3: toast(T.page.links[link]); break;
  }
}

static void on_tap(int x, int y) {
  if (g.menu_link >= 0) { menu_tap(x, y); return; }
  if (y < BAR) {
    if (x < 54) { g.mode = MODE_PAGE; go_back(); }
    else if (x < 102) { g.mode = MODE_PAGE; go_fwd(); }
    else if (x < 150) { g.mode = MODE_PAGE; reload(); }
    else if (x < 198) { g.mode = MODE_PAGE; go_home(); }
    else if (x < SW - 144) { g.mode = MODE_PAGE; open_address_bar(); }
    else if (x < SW - 96) bm_toggle_current();
    else if (x < SW - 48) g.mode = (g.mode == MODE_BOOKMARKS) ? MODE_PAGE : MODE_BOOKMARKS;
    else g.mode = (g.mode == MODE_TABS) ? MODE_PAGE : MODE_TABS;
    return;
  }
  if (g.mode == MODE_TABS) {
    int count = ntabs + (ntabs < MAXTABS ? 1 : 0);
    for (int i = 0; i < count; i++) {
      int cx = card_x(i), cy = card_y(i);
      if (x < cx || x >= cx + CARD_W || y < cy || y >= cy + CARD_H) continue;
      if (i == ntabs) { new_tab(); g.mode = MODE_PAGE; }
      else if (x >= cx + CARD_W - 56 && y < cy + 56) close_tab(i);
      else { act = i; g.mode = MODE_PAGE; }
      return;
    }
    return;
  }
  if (g.mode == MODE_BOOKMARKS) {
    for (int i = 0; i < nbm; i++) {
      float ry = BAR + 10 + i * BM_ROW - g.bscroll;
      if (y < ry || y >= ry + BM_ROW - 8) continue;
      if (x >= SW - 110) bm_delete(i);
      else {
        char u[512];
        snprintf(u, sizeof(u), "%s", bm[i].url);
        g.mode = MODE_PAGE;
        navigate(u, 1);
      }
      return;
    }
    return;
  }
  if (!T.cur[0]) {
    for (int i = 0; i < NTILES; i++) {
      int tx = tile_x(i), ty = tile_y(i);
      if (x >= tx && x < tx + TILE_W && y >= ty && y < ty + TILE_H) { navigate(tiles[i].url, 1); return; }
    }
    return;
  }
  int link = link_at(x, y);
  if (link >= 0) open_link(link, 0);
}

/* ---------- main ---------- */

int main(void) {
  vita2d_init();
  vita2d_set_clear_color(C_BG);
  g.pgf = vita2d_load_default_pgf();
  g.menu_link = g.press_link = -1;
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  net_init();
  settings_load();
  bm_load();
  gh_load_token();

  int touching = 0, moved = 0, held = 0, long_fired = 0;
  int sx = 0, sy = 0, lx = 0, ly = 0;
  int pinching = 0, pinch_used = 0;
  float pinch_d0 = 1, pinch_z0 = 1, pinch_anchor = 0;

  for (;;) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pressed = pad.buttons & ~g.old_buttons;
    g.old_buttons = pad.buttons;

    if (g.menu_link >= 0) {
      if (pressed & SCE_CTRL_CIRCLE) g.menu_link = -1;
      pressed = 0;
    }
    if (pressed & SCE_CTRL_START) g.mode = (g.mode == MODE_TABS) ? MODE_PAGE : MODE_TABS;
    if (pressed & SCE_CTRL_CROSS && g.mode == MODE_PAGE) bm_toggle_current();
    if (pressed & SCE_CTRL_TRIANGLE) { g.mode = MODE_PAGE; open_address_bar(); }
    if (pressed & SCE_CTRL_SQUARE) { g.mode = MODE_PAGE; reload(); }
    if (pressed & SCE_CTRL_CIRCLE) { if (g.mode != MODE_PAGE) g.mode = MODE_PAGE; else go_back(); }
    if (pressed & SCE_CTRL_LTRIGGER) { g.mode = MODE_PAGE; go_back(); }
    if (pressed & SCE_CTRL_RTRIGGER) { g.mode = MODE_PAGE; go_fwd(); }
    if (pressed & SCE_CTRL_SELECT) { g.mode = MODE_PAGE; go_home(); }

    float *sc = (g.mode == MODE_BOOKMARKS) ? &g.bscroll : &T.scroll;
    if (pad.buttons & SCE_CTRL_UP)   *sc -= 14;
    if (pad.buttons & SCE_CTRL_DOWN) *sc += 14;
    int ry = pad.ry - 128;
    if (abs(ry) > 24) *sc += ry * 0.12f;

    SceTouchData t;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
    int page_gestures = (g.mode == MODE_PAGE && T.cur[0] && g.menu_link < 0);

    if (t.reportNum >= 2 && page_gestures) {
      /* ---- pinch to zoom ---- */
      float x0 = t.report[0].x / 2.0f, y0 = t.report[0].y / 2.0f;
      float x1 = t.report[1].x / 2.0f, y1 = t.report[1].y / 2.0f;
      float d = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
      float midy = (y0 + y1) / 2;
      if (!pinching) {
        pinching = pinch_used = 1;
        pinch_d0 = d > 20 ? d : 20;
        pinch_z0 = g.zoom;
        pinch_anchor = (midy - BAR - MARGIN / 2 + T.scroll) / T.zoom;   /* content point under fingers */
        g.edge = 0; g.pull = 0; g.press_link = -1;
        T.vel = 0;
      }
      float nz = pinch_z0 * d / pinch_d0;
      if (nz < ZOOM_MIN) nz = ZOOM_MIN;
      if (nz > ZOOM_MAX) nz = ZOOM_MAX;
      if (fabsf(nz - T.zoom) > 0.05f) {
        g.zoom = nz;
        relayout();
        T.scroll = pinch_anchor * T.zoom - (midy - BAR - MARGIN / 2);
      }
      touching = 1; moved = 1;
    } else if (t.reportNum > 0) {
      int x = t.report[0].x / 2, y = t.report[0].y / 2;
      if (pinching) { pinching = 0; lx = x; ly = y; }   /* one finger lifted: avoid a jump */
      if (!touching) {
        touching = 1; moved = 0; held = 0; long_fired = 0; pinch_used = 0;
        sx = lx = x; sy = ly = y;
        T.vel = 0;
        g.edge = 0; g.edge_dx = 0;
        if (page_gestures && y >= BAR) {
          if (x < EDGE_W) g.edge = -1;
          else if (x > SW - EDGE_W) g.edge = 1;
          g.press_link = link_at(x, y);
        }
      } else {
        held++;
        if (abs(x - sx) > 12 || abs(y - sy) > 12) { moved = 1; g.press_link = -1; }
        /* an edge touch that turns into a vertical drag becomes a scroll */
        if (g.edge && abs(y - sy) > 24 && abs(y - sy) > abs(x - sx)) g.edge = 0;
        if (g.edge) g.edge_dx = (float)(x - sx);
        else if (moved && sy >= BAR && g.mode != MODE_TABS && !pinch_used) {
          float dy = (float)(y - ly);
          if (g.mode == MODE_BOOKMARKS) g.bscroll -= dy;
          else if (page_gestures && (g.pull > 0 || (T.scroll <= 0 && dy > 0))) {
            g.pull += dy > 0 ? dy * 0.5f : dy;          /* pulling down at the top */
            if (g.pull < 0) g.pull = 0;
          } else { T.scroll -= dy; T.vel = -dy; }
        }
        /* long press on a link opens the menu */
        if (!moved && !long_fired && held >= LONG_PRESS && page_gestures && g.press_link >= 0) {
          long_fired = 1;
          g.menu_link = g.press_link;
          g.press_link = -1;
          g.edge = 0;
        }
        lx = x; ly = y;
      }
    } else if (touching) {
      touching = 0;
      g.press_link = -1;
      if (pinching || pinch_used) { pinching = pinch_used = 0; relayout(); settings_save(); }
      else if (g.edge) {
        int e = g.edge;
        float dx = g.edge_dx;
        g.edge = 0;
        if (!moved) on_tap(sx, sy);
        else if (e < 0 && dx >= EDGE_TRIGGER) go_back();
        else if (e > 0 && dx <= -EDGE_TRIGGER) go_fwd();
      } else if (g.pull > 0) {
        int go = g.pull >= PULL_TRIGGER;
        g.pull = 0;
        if (go) reload();
      } else if (!moved && !long_fired) on_tap(sx, sy);
    } else if (g.mode == MODE_PAGE && fabsf(T.vel) > 0.3f) {
      T.scroll += T.vel;                                 /* momentum */
      T.vel *= 0.94f;
    }

    float ms = max_scroll();
    if (T.scroll < 0) { T.scroll = 0; T.vel = 0; }
    if (T.scroll > ms) { T.scroll = ms; T.vel = 0; }
    float bms = bm_max_scroll();
    if (g.bscroll < 0) g.bscroll = 0;
    if (g.bscroll > bms) g.bscroll = bms;

    render();
  }
  return 0;
}
