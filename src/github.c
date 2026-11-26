#include "github.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>
#include <ctype.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>

#define DATA_DIR   "ux0:data/VitaSurf"
#define TOKEN_FILE DATA_DIR "/github_token.txt"
#define API        "https://api.github.com"
#define MAX_FILE   (200 * 1024)

static char token[256];

/* ---------- token ---------- */

void gh_load_token(void) {
  FILE *f = fopen(TOKEN_FILE, "r");
  if (!f) return;
  if (fgets(token, sizeof(token), f)) token[strcspn(token, "\r\n \t")] = 0;
  fclose(f);
}

const char *gh_token(void) { return token; }

void gh_set_token(const char *tok) {
  snprintf(token, sizeof(token), "%s", tok ? tok : "");
  token[strcspn(token, "\r\n \t")] = 0;
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA_DIR, 0777);
  if (!token[0]) { sceIoRemove(TOKEN_FILE); return; }
  FILE *f = fopen(TOKEN_FILE, "w");
  if (f) { fprintf(f, "%s\n", token); fclose(f); }
}

/* ---------- small string builder ---------- */

typedef struct { char *d; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s) {
  size_t l = strlen(s);
  if (b->n + l + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->n + l + 1) nc *= 2;
    char *nd = realloc(b->d, nc);
    if (!nd) return;
    b->d = nd; b->cap = nc;
  }
  memcpy(b->d + b->n, s, l + 1);
  b->n += l;
}

static void bprintf(Buf *b, const char *fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  bput(b, tmp);
}

/* Escapes text for HTML. nl_br turns newlines into <br>. */
static void besc(Buf *b, const char *s, size_t max, int nl_br) {
  char tmp[8];
  for (size_t i = 0; s[i] && i < max; i++) {
    switch (s[i]) {
      case '<': bput(b, "&lt;"); break;
      case '>': bput(b, "&gt;"); break;
      case '&': bput(b, "&amp;"); break;
      case '"': bput(b, "&quot;"); break;
      case '\r': break;
      case '\n': bput(b, nl_br ? "<br>" : "\n"); break;
      default: tmp[0] = s[i]; tmp[1] = 0; bput(b, tmp);
    }
  }
}

/* Percent-encodes for use in an API path or query (keeps '/' if keep_slash). */
static void enc(char *out, size_t osz, const char *s, int keep_slash) {
  size_t o = 0;
  for (; *s && o + 4 < osz; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/'))
      out[o++] = (char)c;
    else o += snprintf(out + o, osz - o, "%%%02X", c);
  }
  out[o] = 0;
}

static void human_size(char *out, size_t osz, double bytes) {
  if (bytes >= 1024 * 1024) snprintf(out, osz, "%.1f MB", bytes / (1024 * 1024));
  else if (bytes >= 1024) snprintf(out, osz, "%.0f KB", bytes / 1024);
  else snprintf(out, osz, "%.0f bytes", bytes);
}

static char *finish(Buf *b) { if (!b->d) bput(b, ""); return b->d; }

static char *error_page(const char *title, const char *msg) {
  Buf b = {0};
  bput(&b, "<title>GitHub</title><h2>");
  besc(&b, title, 200, 0);
  bput(&b, "</h2><p>");
  besc(&b, msg, 600, 1);
  bput(&b, "</p><p><a href=\"gh:\">GitHub home</a></p>");
  return finish(&b);
}

/* ---------- API calls ---------- */

/* Calls the API. Returns parsed JSON (or raw text if raw != NULL). */
static int api(const char *path, const char *accept, JVal **json, char **raw, char *err, size_t esz) {
  char url[1200];
  snprintf(url, sizeof(url), API "%s", path);
  NetReq r = { url, accept ? accept : "application/vnd.github+json", token };
  NetResp resp;
  if (json) *json = NULL;
  if (raw) *raw = NULL;
  if (net_get(&r, &resp, err, esz) != 0) return -1;

  if (resp.status >= 400) {
    JVal *e = json_parse(resp.body);
    const char *m = e ? jstr(e, "message") : "";
    if (resp.status == 401) snprintf(err, esz, "GitHub rejected your token. Sign out and sign in again.");
    else if (resp.status == 403 && strstr(m, "rate limit"))
      snprintf(err, esz, "GitHub's limit was reached (60 requests per hour without signing in). "
                         "Sign in with a token to get 5000 per hour.");
    else if (resp.status == 404)
      snprintf(err, esz, "Not found.%s", token[0] ? "" : " Private repositories need you to sign in.");
    else snprintf(err, esz, "GitHub error %ld: %s", resp.status, m[0] ? m : "unknown");
    json_free(e);
    free(resp.body);
    return (int)resp.status;
  }
  if (raw) { *raw = resp.body; return 0; }
  *json = json_parse(resp.body);
  free(resp.body);
  if (!*json) { snprintf(err, esz, "GitHub sent something unexpected."); return -1; }
  return 0;
}

/* ---------- address parsing ---------- */

int gh_is(const char *url) { return strncmp(url, "gh:", 3) == 0; }
int gh_is_download(const char *url) { return strncmp(url, "ghdl:", 5) == 0; }

/* Splits "OWNER/REPO/rest?ref=X" */
static int split_repo(const char *s, char *owner, char *repo, char *rest, char *ref) {
  char tmp[900];
  snprintf(tmp, sizeof(tmp), "%s", s);
  ref[0] = rest[0] = 0;
  char *q = strstr(tmp, "?ref=");
  if (q) { snprintf(ref, 200, "%s", q + 5); *q = 0; }
  char *a = strchr(tmp, '/');
  if (!a) return 0;
  *a++ = 0;
  char *b = strchr(a, '/');
  if (b) { *b++ = 0; snprintf(rest, 600, "%s", b); }
  snprintf(owner, 100, "%s", tmp);
  snprintf(repo, 100, "%s", a);
  size_t rl = strlen(rest);
  while (rl && rest[rl - 1] == '/') rest[--rl] = 0;
  return owner[0] && repo[0];
}

int gh_from_web(const char *url, char *out, size_t osz) {
  const char *p = NULL;
  if (!strncmp(url, "https://github.com", 18)) p = url + 18;
  else if (!strncmp(url, "https://www.github.com", 22)) p = url + 22;
  else return 0;
  if (*p && *p != '/') return 0;
  char path[900];
  snprintf(path, sizeof(path), "%s", *p ? p + 1 : "");
  path[strcspn(path, "?#")] = 0;

  /* decode %20 etc. (we re-encode when calling the API) */
  char *w = path;
  for (char *q = path; *q; q++) {
    if (q[0] == '%' && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2])) {
      char hex[3] = { q[1], q[2], 0 };
      char ch = (char)strtol(hex, NULL, 16);
      *w++ = (ch == '|') ? '_' : ch;
      q += 2;
    } else *w++ = *q;
  }
  *w = 0;

  /* first five parts, then the rest as one piece */
  char *seg[5] = {0};
  int n = 0;
  char *q = path, *rest = NULL;
  while (*q && n < 5) {
    while (*q == '/') q++;
    if (!*q) break;
    seg[n++] = q;
    char *sl = strchr(q, '/');
    if (!sl) { q += strlen(q); break; }
    *sl = 0;
    q = sl + 1;
  }
  if (n == 5 && *q) rest = q;

  if (n == 0) { snprintf(out, osz, "gh:"); return 1; }
  if (n == 1) {
    static const char *reserved[] = {"search", "login", "explore", "settings", "notifications", "topics", "trending", "features", "about", "pricing", NULL};
    for (int i = 0; reserved[i]; i++) if (!strcmp(seg[0], reserved[i])) { snprintf(out, osz, "gh:"); return 1; }
    snprintf(out, osz, "gh:user/%s", seg[0]);
    return 1;
  }
  size_t rl = strlen(seg[1]);
  if (rl > 4 && !strcmp(seg[1] + rl - 4, ".git")) seg[1][rl - 4] = 0;
  if (n >= 4 && (!strcmp(seg[2], "blob") || !strcmp(seg[2], "tree"))) {
    char sub[700];
    snprintf(sub, sizeof(sub), "%s%s%s", n >= 5 ? seg[4] : "", (rest && *rest) ? "/" : "", rest ? rest : "");
    int head = !strcmp(seg[3], "HEAD");
    snprintf(out, osz, "gh:%s/%s/%s/%s%s%s", !strcmp(seg[2], "blob") ? "file" : "files",
             seg[0], seg[1], sub, head ? "" : "?ref=", head ? "" : seg[3]);
    return 1;
  }
  if (n >= 3 && !strcmp(seg[2], "releases")) { snprintf(out, osz, "gh:releases/%s/%s", seg[0], seg[1]); return 1; }
  snprintf(out, osz, "gh:repo/%s/%s", seg[0], seg[1]);
  return 1;
}

/* ---------- pages ---------- */

static void repo_list(Buf *b, JVal *arr) {
  if (!arr || arr->t != J_ARR || arr->nk == 0) { bput(b, "<p>No repositories found.</p>"); return; }
  for (int i = 0; i < arr->nk; i++) {
    JVal *r = &arr->kids[i];
    const char *full = jstr(r, "full_name");
    bprintf(b, "<p><a href=\"gh:repo/%s\">", full);
    besc(b, full, 200, 0);
    bput(b, "</a>");
    if (jbool(r, "private")) bput(b, " (private)");
    const char *d = jstr(r, "description");
    if (d[0]) { bput(b, "<br>"); besc(b, d, 300, 0); }
    bprintf(b, "<br><small>%.0f stars", jnum(r, "stargazers_count"));
    const char *lang = jstr(r, "language");
    if (lang[0]) { bput(b, " - "); besc(b, lang, 40, 0); }
    const char *up = jstr(r, "pushed_at");
    if (up[0]) bprintf(b, " - updated %.10s", up);
    bput(b, "</small></p>");
  }
}

static char *page_home(void) {
  Buf b = {0};
  bput(&b, "<title>GitHub</title><h1>GitHub</h1>");
  bput(&b, "<p><a href=\"gh:searchbox\">Search repositories</a></p>");
  bput(&b, "<p><a href=\"gh:openbox\">Open a user or repository</a></p>");
  if (token[0]) {
    bput(&b, "<p><a href=\"gh:me\">My repositories</a></p>");
    bput(&b, "<p><a href=\"gh:signout\">Sign out</a></p>");
  } else {
    bput(&b, "<p><a href=\"gh:signin\">Sign in with a personal access token</a></p>");
    bput(&b, "<p><small>Without signing in GitHub allows 60 requests per hour, and private repositories are hidden.</small></p>");
  }
  bprintf(&b, "<p><small>Downloads are saved to %s</small></p>", gh_download_dir());
  bput(&b, "<p><small>Tip: typing a github.com address also opens it here.</small></p>");
  return finish(&b);
}

static char *page_search(const char *q) {
  char eq[400], path[600], err[400];
  /* q is already URL-encoded from the address */
  snprintf(eq, sizeof(eq), "%s", q);
  snprintf(path, sizeof(path), "/search/repositories?q=%s&per_page=30", eq);
  JVal *j;
  if (api(path, NULL, &j, NULL, err, sizeof(err)) != 0) return error_page("Search failed", err);
  Buf b = {0};
  bput(&b, "<title>GitHub search</title><h2>Search results</h2>");
  bprintf(&b, "<p><small>%.0f repositories found</small> <a href=\"gh:searchbox\">New search</a></p>", jnum(j, "total_count"));
  repo_list(&b, jget(j, "items"));
  json_free(j);
  return finish(&b);
}

static char *page_user(const char *name, int me) {
  char path[300], err[400], en[120];
  enc(en, sizeof(en), name, 0);
  if (me) snprintf(path, sizeof(path), "/user/repos?sort=pushed&per_page=60");
  else snprintf(path, sizeof(path), "/users/%s/repos?sort=pushed&per_page=60", en);
  JVal *j;
  if (api(path, NULL, &j, NULL, err, sizeof(err)) != 0) return error_page(me ? "My repositories" : name, err);
  Buf b = {0};
  if (me) bput(&b, "<title>My repositories</title><h1>My repositories</h1>");
  else { bput(&b, "<title>"); besc(&b, name, 100, 0); bput(&b, "</title><h1>"); besc(&b, name, 100, 0); bput(&b, "</h1>"); }
  repo_list(&b, j);
  json_free(j);
  return finish(&b);
}

static char *page_repo(const char *o, const char *r, char *base, size_t bsz) {
  char path[300], err[400];
  snprintf(path, sizeof(path), "/repos/%s/%s", o, r);
  JVal *j;
  if (api(path, NULL, &j, NULL, err, sizeof(err)) != 0) return error_page("Repository", err);
  const char *branch = jstr(j, "default_branch");
  snprintf(base, bsz, "https://github.com/%s/%s/blob/%s/", o, r, branch[0] ? branch : "HEAD");

  Buf b = {0};
  bprintf(&b, "<title>%s/%s</title><h1>%s/%s</h1>", o, r, o, r);
  const char *d = jstr(j, "description");
  if (d[0]) { bput(&b, "<p>"); besc(&b, d, 400, 0); bput(&b, "</p>"); }
  bprintf(&b, "<p><small>%.0f stars - %.0f forks - branch %s%s</small></p>",
          jnum(j, "stargazers_count"), jnum(j, "forks_count"), branch, jbool(j, "private") ? " - private" : "");
  bprintf(&b, "<p><a href=\"gh:files/%s/%s\">Files</a> <a href=\"gh:releases/%s/%s\">Releases</a> "
              "<a href=\"gh:user/%s\">More from %s</a></p><hr>", o, r, o, r, o, o);
  json_free(j);

  char *html = NULL;
  snprintf(path, sizeof(path), "/repos/%s/%s/readme", o, r);
  if (api(path, "application/vnd.github.html", NULL, &html, err, sizeof(err)) == 0 && html) {
    bput(&b, html);
    free(html);
  } else bput(&b, "<p><small>This repository has no README.</small></p>");
  return finish(&b);
}

static int is_markdown(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".markdown"));
}

static char *page_file(const char *o, const char *r, const char *fpath, const char *ref,
                       char *base, size_t bsz) {
  char path[1200], err[400], ep[800];
  enc(ep, sizeof(ep), fpath, 1);
  snprintf(path, sizeof(path), "/repos/%s/%s/contents%s%s%s%s", o, r, ep[0] ? "/" : "", ep,
           ref[0] ? "?ref=" : "", ref);                 /* no trailing slash: the API rejects it */
  const char *name = strrchr(fpath, '/') ? strrchr(fpath, '/') + 1 : fpath;
  int md = is_markdown(name);
  char *raw;
  if (api(path, md ? "application/vnd.github.html" : "application/vnd.github.raw", NULL, &raw, err, sizeof(err)) != 0)
    return error_page(name, err);

  /* relative links in a markdown file are relative to its folder */
  char dir[700];
  snprintf(dir, sizeof(dir), "%s", fpath);
  char *slash = strrchr(dir, '/');
  if (slash) slash[1] = 0; else dir[0] = 0;
  snprintf(base, bsz, "https://github.com/%s/%s/blob/%s/%s", o, r, ref[0] ? ref : "HEAD", dir);

  Buf b = {0};
  bput(&b, "<title>"); besc(&b, name, 100, 0); bput(&b, "</title>");
  bprintf(&b, "<p><a href=\"gh:repo/%s/%s\">%s/%s</a> / ", o, r, o, r);
  besc(&b, fpath, 300, 0);
  bput(&b, "</p><hr>");
  size_t len = strlen(raw);
  if (md) bput(&b, raw);
  else if (memchr(raw, 0, len < 8000 ? len : 8000) || len == 0) {
    bput(&b, "<p><small>This file can't be shown as text.</small></p>");
  } else {
    bput(&b, "<pre>");
    besc(&b, raw, MAX_FILE, 0);
    bput(&b, "</pre>");
    if (len > MAX_FILE) bput(&b, "<p><small>(file shortened)</small></p>");
  }
  free(raw);
  return finish(&b);
}

static int cmp_entries(const void *a, const void *b) {
  const JVal *x = a, *y = b;
  int dx = !strcmp(jstr(x, "type"), "dir"), dy = !strcmp(jstr(y, "type"), "dir");
  if (dx != dy) return dy - dx;
  return strcasecmp(jstr(x, "name"), jstr(y, "name"));
}

static char *page_files(const char *o, const char *r, const char *fpath, const char *ref,
                        char *base, size_t bsz) {
  char path[1200], err[400], ep[800];
  enc(ep, sizeof(ep), fpath, 1);
  snprintf(path, sizeof(path), "/repos/%s/%s/contents%s%s%s%s", o, r, ep[0] ? "/" : "", ep,
           ref[0] ? "?ref=" : "", ref);                 /* no trailing slash: the API rejects it */
  JVal *j;
  if (api(path, NULL, &j, NULL, err, sizeof(err)) != 0) return error_page("Files", err);
  if (j->t != J_ARR) { json_free(j); return page_file(o, r, fpath, ref, base, bsz); }
  qsort(j->kids, j->nk, sizeof(JVal), cmp_entries);

  char refq[220] = "";
  if (ref[0]) snprintf(refq, sizeof(refq), "?ref=%s", ref);
  Buf b = {0};
  bprintf(&b, "<title>%s/%s files</title>", o, r);
  bprintf(&b, "<p><a href=\"gh:repo/%s/%s\">%s/%s</a> / ", o, r, o, r);
  besc(&b, fpath, 300, 0);
  bput(&b, "</p><hr><ul>");
  if (fpath[0]) {
    char up[700];
    snprintf(up, sizeof(up), "%s", fpath);
    char *s = strrchr(up, '/');
    if (s) *s = 0; else up[0] = 0;
    bprintf(&b, "<li><a href=\"gh:files/%s/%s/", o, r);
    besc(&b, up, 600, 0);
    bprintf(&b, "%s\">.. (up a folder)</a></li>", refq);
  }
  for (int i = 0; i < j->nk; i++) {
    JVal *e = &j->kids[i];
    int dir = !strcmp(jstr(e, "type"), "dir");
    char sz[32];
    bprintf(&b, "<li><a href=\"gh:%s/%s/%s/", dir ? "files" : "file", o, r);
    besc(&b, jstr(e, "path"), 600, 0);          /* addresses keep plain paths */
    bprintf(&b, "%s\">", refq);
    besc(&b, jstr(e, "name"), 200, 0);
    bput(&b, dir ? "/</a>" : "</a>");
    if (!dir) { human_size(sz, sizeof(sz), jnum(e, "size")); bprintf(&b, " <small>%s</small>", sz); }
    bput(&b, "</li>");
  }
  bput(&b, "</ul>");
  json_free(j);
  return finish(&b);
}

static char *page_releases(const char *o, const char *r) {
  char path[300], err[400];
  snprintf(path, sizeof(path), "/repos/%s/%s/releases?per_page=10", o, r);
  JVal *j;
  if (api(path, NULL, &j, NULL, err, sizeof(err)) != 0) return error_page("Releases", err);
  Buf b = {0};
  bprintf(&b, "<title>%s/%s releases</title><p><a href=\"gh:repo/%s/%s\">%s/%s</a></p><h1>Releases</h1>", o, r, o, r, o, r);
  if (j->t != J_ARR || j->nk == 0) bput(&b, "<p>No releases yet.</p>");
  for (int i = 0; j->t == J_ARR && i < j->nk; i++) {
    JVal *rel = &j->kids[i];
    const char *tag = jstr(rel, "tag_name"), *name = jstr(rel, "name");
    bput(&b, "<h2>"); besc(&b, name[0] ? name : tag, 150, 0); bput(&b, "</h2>");
    bprintf(&b, "<p><small>%s - %.10s%s</small></p>", tag, jstr(rel, "published_at"),
            jbool(rel, "prerelease") ? " - pre-release" : "");
    JVal *assets = jget(rel, "assets");
    if (assets && assets->t == J_ARR && assets->nk) {
      bput(&b, "<ul>");
      for (int k = 0; k < assets->nk; k++) {
        JVal *a = &assets->kids[k];
        char sz[32];
        human_size(sz, sizeof(sz), jnum(a, "size"));
        bprintf(&b, "<li><a href=\"ghdl:%s|%s|%.0f|%s|%s\">Download ", o, r, jnum(a, "id"), tag, jstr(a, "name"));
        besc(&b, jstr(a, "name"), 200, 0);
        bprintf(&b, "</a> <small>%s</small></li>", sz);
      }
      bput(&b, "</ul>");
    } else bput(&b, "<p><small>No files attached.</small></p>");
    const char *body = jstr(rel, "body");
    if (body[0]) { bput(&b, "<p>"); besc(&b, body, 1500, 1); if (strlen(body) > 1500) bput(&b, "..."); bput(&b, "</p>"); }
    bput(&b, "<hr>");
  }
  json_free(j);
  return finish(&b);
}

char *gh_render(const char *url, char *base, size_t bsz) {
  const char *p = url + 3;
  char o[100], r[100], rest[600], ref[200];
  snprintf(base, bsz, "%s", url);
  if (!*p) return page_home();
  if (!strncmp(p, "search?q=", 9)) return page_search(p + 9);
  if (!strncmp(p, "user/", 5)) return page_user(p + 5, 0);
  if (!strcmp(p, "me")) {
    if (!token[0]) return error_page("My repositories", "Sign in first.");
    return page_user("", 1);
  }
  const char *kinds[] = {"repo/", "files/", "file/", "releases/"};
  for (int k = 0; k < 4; k++) {
    size_t kl = strlen(kinds[k]);
    if (strncmp(p, kinds[k], kl)) continue;
    if (!split_repo(p + kl, o, r, rest, ref)) break;
    switch (k) {
      case 0: return page_repo(o, r, base, bsz);
      case 1: return page_files(o, r, rest, ref, base, bsz);
      case 2: return page_file(o, r, rest, ref, base, bsz);
      default: return page_releases(o, r);
    }
  }
  return error_page("GitHub", "This GitHub address isn't supported.");
}

/* ---------- downloads ---------- */

const char *gh_download_dir(void) {
  static char dir[64];
  if (!dir[0]) {
    SceIoStat st;
    const char *root = (sceIoGetstat("uma0:", &st) >= 0) ? "uma0:" : "ux0:";
    snprintf(dir, sizeof(dir), "%sdata/VitaSurf/downloads", root);
  }
  return dir;
}

int gh_download(const char *url, NetProgress progress, void *user,
                char *saved, size_t ssz, char *err, size_t esz) {
  char tmp[900];
  snprintf(tmp, sizeof(tmp), "%s", url + 5);
  char *f[5] = {0};
  int n = 0;
  char *save = NULL;
  for (char *t = strtok_r(tmp, "|", &save); t && n < 5; t = strtok_r(NULL, "|", &save)) f[n++] = t;
  if (n < 5) { snprintf(err, esz, "Bad download link"); return -1; }
  const char *o = f[0], *r = f[1], *id = f[2], *tag = f[3], *name = f[4];
  if (strchr(name, '/') || strchr(name, ':') || !strcmp(name, "..")) { snprintf(err, esz, "Unsafe file name"); return -1; }

  /* create the folder chain */
  const char *dir = gh_download_dir();
  char part[64];
  snprintf(part, sizeof(part), "%.5sdata", dir);                 sceIoMkdir(part, 0777);
  snprintf(part, sizeof(part), "%.5sdata/VitaSurf", dir);        sceIoMkdir(part, 0777);
  sceIoMkdir(dir, 0777);
  snprintf(saved, ssz, "%s/%s", dir, name);

  char link[700], et[200], en[300];
  if (token[0]) {   /* API link works for private repos too */
    snprintf(link, sizeof(link), API "/repos/%s/%s/releases/assets/%s", o, r, id);
    return net_download(link, saved, "application/octet-stream", token, progress, user, err, esz);
  }
  enc(et, sizeof(et), tag, 0);
  enc(en, sizeof(en), name, 0);
  snprintf(link, sizeof(link), "https://github.com/%s/%s/releases/download/%s/%s", o, r, et, en);
  return net_download(link, saved, NULL, NULL, progress, user, err, esz);
}
