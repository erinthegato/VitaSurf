#ifndef HTML_H
#define HTML_H

/* Styles: 0 normal, 1 h1, 2 h2, 3 dim (image alt), 4 h3 */
#define STYLE_COUNT 5
#define LINE_H 26
extern const float html_style_scale[STYLE_COUNT];

typedef struct {
  char *text;
  int link;      /* index into Page.links, or -1 */
  int style;
  int breaks;    /* line breaks before this word (0, 1 or 2) */
  int x, y, w, h;
} Word;

typedef struct {
  Word *words;
  int nwords, cap;
  char **links;
  int nlinks, lcap;
  char title[128];
  int height;
} Page;

typedef int (*MeasureFn)(const char *text, int style);

void html_parse(Page *p, const char *html, const char *base_url);
void html_layout(Page *p, MeasureFn measure, int width);
void html_free(Page *p);

#endif
