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
#define C_ACCENT RGBA8(90, 140, 255, 255)
#define C_TILE   RGBA8(36, 36, 44, 255)

typedef struct {
  Page page;
  char cur[1024];            /* "" = home screen */
  char hist[HMAX][1024];
  int hn, hp;
  float scroll, vel;
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
  char toast[64];
  int toast_t;
} g;

static const struct { const char *name; const char *url; } tiles[4] = {
  {"Wikipedia",      "https://en.m.wikipedia.org"},
  {"DuckDuckGo",     "https://lite.duckduckgo.com/lite/"},
  {"Hacker News",    "https://news.ycombinator.com"},
  {"NPR Text",       "https://text.npr.org"},
};

/* ---------- helpers ---------- */

static int text_w(const char *t, float sc) { return vita2d_pgf_text_width(g.pgf, sc, t); }
static int measure(const char *t, int style) { return text_w(t, html_style_scale[style]); }

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
  const char *p = strstr(url, "://");
  return p ? p + 3 : url;
}

/* ---------- bookmarks ---------- */

#define BM_DIR  "ux0:data/VitaSurf"
#define BM_FILE BM_DIR "/bookmarks.txt"

static void bm_load(void) {
  FILE *f = fopen(BM_FILE, "r");
  if (!f) return;
  char line[700];
  while (nbm < MAXBM && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    snprintf(bm[nbm].url, sizeof(bm[nbm].url), "%s", line);
    snprintf(bm[nbm].title, sizeof(bm[nbm].title), "%s", tab + 1);
    nbm++;
  }
  fclose(f);
}

static void bm_save(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(BM_DIR, 0777);
  FILE *f = fopen(BM_FILE, "w");
  if (!f) return;
  for (int i = 0; i < nbm; i++) fprintf(f, "%s\t%s\n", bm[i].url, bm[i].title);
  fclose(f);
}

static int bm_find(const char *url) {
  for (int i = 0; i < nbm; i++) if (strcmp(bm[i].url, url) == 0) return i;
  return -1;
}

static void toast(const char *msg) { snprintf(g.toast, sizeof(g.toast), "%s", msg); g.toast_t = 120; }

static void bm_toggle_current(void) {
  if (!T.cur[0]) return;
  int i = bm_find(T.cur);
  if (i >= 0) {
    memmove(&bm[i], &bm[i + 1], (nbm - i - 1) * sizeof(Bookmark));
    nbm--;
    toast("Bookmark removed");
  } else if (nbm < MAXBM) {
    snprintf(bm[nbm].url, sizeof(bm[nbm].url), "%s", T.cur);
    const char *title = T.page.title[0] ? T.page.title : host_of(T.cur);
    snprintf(bm[nbm].title, sizeof(bm[nbm].title), "%s", title);
    for (char *c = bm[nbm].title; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
    nbm++;
    toast("Bookmark added");
  } else {
    toast("Bookmark list is full");
    return;
  }
  bm_save();
}

static void bm_delete(int i) {
  if (i < 0 || i >= nbm) return;
  memmove(&bm[i], &bm[i + 1], (nbm - i - 1) * sizeof(Bookmark));
  nbm--;
  bm_save();
}

static float bm_max_scroll(void) {
  float m = nbm * BM_ROW + 20 - (SH - BAR);
  return m > 0 ? m : 0;
}

/* ---------- tabs ---------- */

static void new_tab(void) {
  if (ntabs >= MAXTABS) { toast("Tab limit reached"); return; }
  memset(&tabs[ntabs], 0, sizeof(Tab));
  act = ntabs++;
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

/* tab cards: 3 columns */
#define CARD_W 280
#define CARD_H 110
static int card_x(int i) { return 40 + (i % 3) * (CARD_W + 20); }
static int card_y(int i) { return BAR + 24 + (i / 3) * (CARD_H + 20); }

/* ---------- drawing ---------- */

static void draw_home(void) {
  const char *title = "VitaSurf";
  text_at((SW - text_w(title, 2.0f)) / 2, BAR + 90, C_TEXT, 2.0f, title);
  const char *sub = "Tap a tile, or press Triangle to search or type an address";
  text_at((SW - text_w(sub, 1.0f)) / 2, BAR + 125, C_DIM, 1.0f, sub);
  for (int i = 0; i < 4; i++) {
    int x = 168 + (i % 2) * 324, y = 200 + (i / 2) * 144;
    vita2d_draw_rectangle(x, y, 300, 120, C_TILE);
    vita2d_draw_rectangle(x, y, 6, 120, C_ACCENT);
    text_at(x + 28, y + 56, C_TEXT, 1.3f, tiles[i].name);
    text_at(x + 28, y + 88, C_DIM, 0.85f, host_of(tiles[i].url));
  }
}

static void draw_page(void) {
  vita2d_set_clip_rectangle(0, BAR, SW, SH);
  vita2d_enable_clipping();
  for (int i = 0; i < T.page.nwords; i++) {
    Word *w = &T.page.words[i];
    float sy = BAR + MARGIN / 2 + w->y - T.scroll;
    if (sy + w->h < BAR || sy > SH) continue;
    float sc = html_style_scale[w->style];
    unsigned col = w->link >= 0 ? C_LINK : (w->style == 3 ? C_DIM : C_TEXT);
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
    const char *m = "No bookmarks yet. Tap the star to save the current page.";
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

  if (T.cur[0] && !g.loading && strncmp(T.cur, "https://", 8) == 0)
    vita2d_draw_rectangle(0, BAR - 2, SW, 2, RGBA8(70, 190, 120, 255));  /* secure */
  if (g.loading) vita2d_draw_rectangle(0, BAR - 2, SW, 2, C_ACCENT);

  if (g.toast_t > 0) {
    g.toast_t--;
    int tw = text_w(g.toast, 1.0f);
    vita2d_draw_rectangle((SW - tw) / 2 - 20, SH - 70, tw + 40, 40, RGBA8(60, 60, 72, 240));
    text_at((SW - tw) / 2, SH - 43, C_TEXT, 1.0f, g.toast);
  }

  vita2d_end_drawing();
  vita2d_common_dialog_update();
  vita2d_swap_buffers();
}

/* ---------- navigation ---------- */

static void navigate(const char *url, int push) {
  char target[1024];
  snprintf(target, sizeof(target), "%s", url);

  g.loading = 1;
  render();

  char *buf = NULL; size_t len = 0;
  char fin[1024], err[320];
  html_free(&T.page);
  if (net_fetch(target, &buf, &len, fin, sizeof(fin), err, sizeof(err)) == 0) {
    snprintf(T.cur, sizeof(T.cur), "%s", fin);
    html_parse(&T.page, buf, fin);
    free(buf);
  } else {
    snprintf(T.cur, sizeof(T.cur), "%s", target);
    char msg[700];
    snprintf(msg, sizeof(msg), "<h2>Could not load page</h2><p>%s</p><p>%s</p>", target, err);
    html_parse(&T.page, msg, target);
  }
  html_layout(&T.page, measure, SW - 2 * MARGIN);
  T.scroll = 0; T.vel = 0;

  if (push) {
    if (T.hn > 0) T.hn = T.hp + 1;
    if (T.hn == HMAX) { memmove(T.hist[0], T.hist[1], (HMAX - 1) * 1024); T.hn--; }
    snprintf(T.hist[T.hn], 1024, "%s", T.cur);
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

/* ---------- address bar / search ---------- */

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

static int ime_input(const char *initial, char *out, size_t osz) {
  static SceWChar16 title[16], init[512], input[512 + 1];
  const char *t = "Search or enter address";
  int i;
  for (i = 0; t[i] && i < 15; i++) title[i] = t[i];
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
  return ok && out[0];
}

static void open_address_bar(void) {
  char in[600];
  if (!ime_input(T.cur, in, sizeof(in))) return;
  char *s = in;
  while (*s == ' ') s++;
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
  if (!n) return;

  char url[1100];
  if (strstr(s, "://")) snprintf(url, sizeof(url), "%s", s);
  else if (!strchr(s, ' ') && strchr(s, '.')) snprintf(url, sizeof(url), "https://%s", s);
  else {
    char q[700];
    url_encode(s, q, sizeof(q));
    snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", q);
  }
  navigate(url, 1);
}

/* ---------- touch ---------- */

static void on_tap(int x, int y) {
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
      else if (x >= cx + CARD_W - 56 && y < cy + 56) {
        close_tab(i);
        if (ntabs == 1 && !T.cur[0] && i == 0) { /* last tab reset */ }
      } else { act = i; g.mode = MODE_PAGE; }
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
    for (int i = 0; i < 4; i++) {
      int tx = 168 + (i % 2) * 324, ty = 200 + (i / 2) * 144;
      if (x >= tx && x < tx + 300 && y >= ty && y < ty + 120) { navigate(tiles[i].url, 1); return; }
    }
    return;
  }
  int cx = x - MARGIN;
  float cy = y - BAR - MARGIN / 2 + T.scroll;
  for (int i = 0; i < T.page.nwords; i++) {
    Word *w = &T.page.words[i];
    if (w->link < 0) continue;
    if (cx >= w->x - 4 && cx <= w->x + w->w + 4 && cy >= w->y - 4 && cy <= w->y + w->h + 4) {
      char u[1024];
      snprintf(u, sizeof(u), "%s", T.page.links[w->link]);
      navigate(u, 1);
      return;
    }
  }
}

/* ---------- main ---------- */

int main(void) {
  vita2d_init();
  vita2d_set_clear_color(C_BG);
  g.pgf = vita2d_load_default_pgf();
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  net_init();
  bm_load();

  unsigned old = 0;
  int touching = 0, moved = 0, sx = 0, sy = 0, lx = 0, ly = 0;

  for (;;) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pressed = pad.buttons & ~old;
    old = pad.buttons;

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
    if (t.reportNum > 0) {
      int x = t.report[0].x / 2, y = t.report[0].y / 2;
      if (!touching) { touching = 1; moved = 0; sx = lx = x; sy = ly = y; T.vel = 0; }
      else {
        if (abs(x - sx) > 12 || abs(y - sy) > 12) moved = 1;
        if (moved && sy >= BAR && g.mode != MODE_TABS) {
          float dy = (float)(y - ly);
          if (g.mode == MODE_BOOKMARKS) g.bscroll -= dy;
          else { T.scroll -= dy; T.vel = -dy; }
        }
        lx = x; ly = y;
      }
    } else if (touching) {
      touching = 0;
      if (!moved) on_tap(sx, sy);
    } else if (g.mode == MODE_PAGE && fabsf(T.vel) > 0.3f) {
      T.scroll += T.vel;
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
