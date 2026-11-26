#include "html.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

const float html_style_scale[STYLE_COUNT] = {1.0f, 1.7f, 1.35f, 0.85f, 1.15f};

/* ---------- small helpers ---------- */

static int ci_starts(const char *s, const char *pre) {
  while (*pre) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    s++; pre++;
  }
  return 1;
}

static const char *ci_find(const char *s, const char *needle) {
  for (; *s; s++) if (ci_starts(s, needle)) return s;
  return NULL;
}

/* Reads attribute value from a tag's text into out. Returns 1 if found. */
static int get_attr(const char *tag, const char *name, char *out, size_t outsz) {
  size_t nl = strlen(name);
  const char *p = tag;
  while ((p = ci_find(p, name))) {
    int ok_before = (p > tag && isspace((unsigned char)p[-1]));
    const char *q = p + nl;
    p++;
    if (!ok_before) continue;
    while (isspace((unsigned char)*q)) q++;
    if (*q != '=') continue;
    q++;
    while (isspace((unsigned char)*q)) q++;
    char quote = 0;
    if (*q == '"' || *q == '\'') quote = *q++;
    size_t i = 0;
    while (*q && i + 1 < outsz) {
      if (quote ? *q == quote : (isspace((unsigned char)*q) || *q == '>')) break;
      out[i++] = *q++;
    }
    out[i] = 0;
    return 1;
  }
  return 0;
}

static void unescape_amp(char *s) {
  char *w = s;
  while (*s) {
    if (strncmp(s, "&amp;", 5) == 0) { *w++ = '&'; s += 5; }
    else *w++ = *s++;
  }
  *w = 0;
}

/* ---------- URL resolving ---------- */

static char *resolve(const char *base, const char *href) {
  if (ci_starts(href, "http://") || ci_starts(href, "https://")) return strdup(href);
  const char *se = strstr(base, "://");
  if (!se) return strdup(href);
  size_t cap = strlen(base) + strlen(href) + 8;
  char *out = malloc(cap);
  if (!out) return NULL;
  const char *hs = se + 3;
  const char *hend = strchr(hs, '/');
  size_t olen = hend ? (size_t)(hend - base) : strlen(base);

  if (href[0] == '/' && href[1] == '/') {
    snprintf(out, cap, "%.*s%s", (int)(se - base + 1), base, href);
  } else if (href[0] == '/') {
    snprintf(out, cap, "%.*s%s", (int)olen, base, href);
  } else if (href[0] == '?') {
    size_t bl = strcspn(base, "?#");
    snprintf(out, cap, "%.*s%s", (int)bl, base, href);
  } else {
    const char *path = base + olen;
    const char *e = path + strcspn(path, "?#");
    const char *ls = NULL;
    for (const char *q = path; q < e; q++) if (*q == '/') ls = q;
    size_t len;
    if (ls) { len = (size_t)(ls - base + 1); memcpy(out, base, len); }
    else { len = olen; memcpy(out, base, len); out[len++] = '/'; }
    for (;;) {
      if (strncmp(href, "./", 2) == 0) href += 2;
      else if (strncmp(href, "../", 3) == 0) {
        href += 3;
        if (len > olen + 1) { len--; while (len > olen && out[len - 1] != '/') len--; }
      } else break;
    }
    out[len] = 0;
    strcat(out, href);
  }
  return out;
}

/* ---------- parser state ---------- */

typedef struct {
  Page *p;
  char w[256];
  int wl;
  int link, style, pend;
  int in_title;
} PS;

static void add_link(Page *p, char *url) {
  if (p->nlinks == p->lcap) {
    p->lcap = p->lcap ? p->lcap * 2 : 64;
    p->links = realloc(p->links, p->lcap * sizeof(char *));
  }
  p->links[p->nlinks++] = url;
}

static void push_word(PS *ps, const char *t) {
  Page *p = ps->p;
  if (p->nwords == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 512;
    p->words = realloc(p->words, p->cap * sizeof(Word));
  }
  Word *w = &p->words[p->nwords++];
  memset(w, 0, sizeof(*w));
  w->text = strdup(t);
  w->link = ps->link;
  w->style = ps->style;
  w->breaks = (p->nwords > 1) ? ps->pend : 0;
  ps->pend = 0;
}

static void flush(PS *ps) {
  if (ps->wl > 0) { ps->w[ps->wl] = 0; push_word(ps, ps->w); ps->wl = 0; }
}

static void brk(PS *ps, int n) {
  flush(ps);
  if (n > ps->pend) ps->pend = n;
}

static void text_char(PS *ps, char c) {
  if (ps->in_title) {
    size_t l = strlen(ps->p->title);
    if (isspace((unsigned char)c)) { if (l && ps->p->title[l - 1] != ' ') c = ' '; else return; }
    if (l + 1 < sizeof(ps->p->title)) { ps->p->title[l] = c; ps->p->title[l + 1] = 0; }
    return;
  }
  if (isspace((unsigned char)c)) { flush(ps); return; }
  if (ps->wl >= 255) flush(ps);
  ps->w[ps->wl++] = c;
}

static void text_str(PS *ps, const char *s) { while (*s) text_char(ps, *s++); }

static const char *map_codepoint(int cp) {
  switch (cp) {
    case 160: return " ";
    case 8216: case 8217: return "'";
    case 8220: case 8221: return "\"";
    case 8211: case 8212: return "-";
    case 8230: return "...";
    case 8226: case 183: return "-";
    case 169: return "(c)";
    default: return "?";
  }
}

/* Handles &...; starting at s. Returns characters consumed (0 if not an entity). */
static int entity(PS *ps, const char *s) {
  const char *semi = memchr(s, ';', 10);
  if (!semi) return 0;
  size_t n = (size_t)(semi - s) - 1;     /* name length without & and ; */
  const char *name = s + 1;
  if (n == 0) return 0;
  if (name[0] == '#') {
    int cp = (name[1] == 'x' || name[1] == 'X') ? (int)strtol(name + 2, NULL, 16)
                                                : atoi(name + 1);
    if (cp > 0 && cp < 128) text_char(ps, (char)cp);
    else text_str(ps, map_codepoint(cp));
    return (int)(semi - s) + 1;
  }
  static const struct { const char *n; const char *r; } tab[] = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
    {"nbsp", " "}, {"copy", "(c)"}, {"rsquo", "'"}, {"lsquo", "'"},
    {"ldquo", "\""}, {"rdquo", "\""}, {"ndash", "-"}, {"mdash", "-"},
    {"hellip", "..."}, {"bull", "-"}, {"middot", "-"},
  };
  for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
    if (strlen(tab[i].n) == n && strncmp(tab[i].n, name, n) == 0) {
      text_str(ps, tab[i].r);
      return (int)(semi - s) + 1;
    }
  }
  return 0;
}

/* ---------- main parser ---------- */

void html_parse(Page *p, const char *html, const char *base) {
  PS ps;
  memset(&ps, 0, sizeof(ps));
  ps.p = p;
  ps.link = -1;
  const char *s = html;

  while (*s) {
    unsigned char c = (unsigned char)*s;

    if (c == '<') {
      if (strncmp(s, "<!--", 4) == 0) {
        const char *e = strstr(s + 4, "-->");
        s = e ? e + 3 : s + strlen(s);
        continue;
      }
      char n1 = s[1];
      if (n1 == '!' || n1 == '?') {
        const char *e = strchr(s, '>');
        s = e ? e + 1 : s + strlen(s);
        continue;
      }
      if (!isalpha((unsigned char)n1) && n1 != '/') { text_char(&ps, '<'); s++; continue; }

      /* find end of tag, respecting quotes */
      const char *q = s + 1;
      char quote = 0;
      while (*q) {
        if (quote) { if (*q == quote) quote = 0; }
        else if (*q == '"' || *q == '\'') quote = *q;
        else if (*q == '>') break;
        q++;
      }
      size_t tl = (size_t)(q - s - 1);
      char tag[1024];
      if (tl >= sizeof(tag)) tl = sizeof(tag) - 1;
      memcpy(tag, s + 1, tl);
      tag[tl] = 0;
      s = *q ? q + 1 : q;

      const char *tp = tag;
      int closing = 0;
      if (*tp == '/') { closing = 1; tp++; }
      char name[16];
      size_t ni = 0;
      while (isalnum((unsigned char)*tp) && ni < sizeof(name) - 1) name[ni++] = (char)tolower((unsigned char)*tp++);
      name[ni] = 0;

      if (!closing && (!strcmp(name, "script") || !strcmp(name, "style"))) {
        const char *end = ci_find(s, !strcmp(name, "script") ? "</script" : "</style");
        if (end) { const char *gt = strchr(end, '>'); s = gt ? gt + 1 : end + strlen(end); }
        else s += strlen(s);
        continue;
      }

      if (!strcmp(name, "title")) { flush(&ps); ps.in_title = !closing; }
      else if (!strcmp(name, "a")) {
        if (closing) { flush(&ps); ps.link = -1; }
        else {
          flush(&ps);
          char href[768];
          ps.link = -1;
          if (get_attr(tag, "href", href, sizeof(href)) && href[0] && href[0] != '#' &&
              !ci_starts(href, "javascript:") && !ci_starts(href, "mailto:") && !ci_starts(href, "tel:")) {
            unescape_amp(href);
            char *r = resolve(base, href);
            if (r) { add_link(p, r); ps.link = p->nlinks - 1; }
          }
        }
      }
      else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        brk(&ps, 2);
        ps.style = closing ? 0 : (name[1] == '1' ? 1 : name[1] == '2' ? 2 : 4);
      }
      else if (!strcmp(name, "p")) brk(&ps, 2);
      else if (!strcmp(name, "li")) {
        brk(&ps, 1);
        if (!closing) { text_str(&ps, "-"); flush(&ps); }
      }
      else if (!strcmp(name, "br") || !strcmp(name, "tr") || !strcmp(name, "hr")) brk(&ps, 1);
      else if (!strcmp(name, "div") || !strcmp(name, "ul") || !strcmp(name, "ol") ||
               !strcmp(name, "table") || !strcmp(name, "section") || !strcmp(name, "article") ||
               !strcmp(name, "header") || !strcmp(name, "footer") || !strcmp(name, "nav") ||
               !strcmp(name, "main") || !strcmp(name, "form") || !strcmp(name, "blockquote") ||
               !strcmp(name, "pre")) brk(&ps, 1);
      else if (!strcmp(name, "td") || !strcmp(name, "th")) flush(&ps);
      else if (!strcmp(name, "img") && !closing) {
        char alt[128];
        if (get_attr(tag, "alt", alt, sizeof(alt)) && alt[0]) {
          flush(&ps);
          int old = ps.style; ps.style = 3;
          char buf[160];
          snprintf(buf, sizeof(buf), "[%s]", alt);
          for (char *b = buf; *b; b++) if ((unsigned char)*b >= 0x80) *b = '?';
          push_word(&ps, buf);
          ps.style = old;
        }
      }
      continue;
    }

    if (c == '&') {
      int used = entity(&ps, s);
      if (used) { s += used; continue; }
      text_char(&ps, '&'); s++; continue;
    }

    if (c >= 0x80) {
      if (c == 0xE2 && (unsigned char)s[1] == 0x80 && s[2]) {
        unsigned char t = (unsigned char)s[2];
        const char *r = (t == 0x98 || t == 0x99) ? "'" :
                        (t == 0x9C || t == 0x9D) ? "\"" :
                        (t == 0x93 || t == 0x94) ? "-" :
                        (t == 0xA6) ? "..." : (t == 0xA2) ? "-" : "?";
        text_str(&ps, r);
        s += 3;
      } else if (c >= 0xC0) {
        text_char(&ps, '?');
        s++;
        while (((unsigned char)*s & 0xC0) == 0x80) s++;
      } else s++;
      continue;
    }

    text_char(&ps, (char)c);
    s++;
  }
  flush(&ps);
}

/* ---------- layout ---------- */

void html_layout(Page *p, MeasureFn measure, int width) {
  int x = 0, y = 0, line_h = 0;
  for (int i = 0; i < p->nwords; i++) {
    Word *w = &p->words[i];
    float sc = html_style_scale[w->style];
    int lh = (int)(LINE_H * sc);
    int ww = measure(w->text, w->style);
    int space = (int)(7 * sc);

    if (i > 0 && w->breaks > 0) {
      y += line_h + (w->breaks >= 2 ? 10 : 0);
      x = 0; line_h = 0;
    } else if (x > 0 && x + ww > width) {
      y += line_h;
      x = 0; line_h = 0;
    }
    w->x = x; w->y = y; w->w = ww; w->h = lh;
    if (lh > line_h) line_h = lh;
    x += ww + space;
  }
  p->height = y + line_h;
}

void html_free(Page *p) {
  for (int i = 0; i < p->nwords; i++) free(p->words[i].text);
  for (int i = 0; i < p->nlinks; i++) free(p->links[i]);
  free(p->words);
  free(p->links);
  memset(p, 0, sizeof(*p));
}
