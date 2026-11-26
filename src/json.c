#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { const char *p; int err; int depth; } JP;

static void ws(JP *j) { while (*j->p && isspace((unsigned char)*j->p)) j->p++; }

static void put_utf8(char **o, unsigned cp) {
  /* Our font only covers ASCII well, so map common characters and drop the rest. */
  if (cp < 128) { *(*o)++ = (char)cp; return; }
  switch (cp) {
    case 0x2018: case 0x2019: *(*o)++ = '\''; return;
    case 0x201C: case 0x201D: *(*o)++ = '"'; return;
    case 0x2013: case 0x2014: *(*o)++ = '-'; return;
    case 0xA0: *(*o)++ = ' '; return;
  }
  if (cp < 0x800) { *(*o)++ = (char)(0xC0 | (cp >> 6)); *(*o)++ = (char)(0x80 | (cp & 63)); }
  else { *(*o)++ = (char)(0xE0 | (cp >> 12)); *(*o)++ = (char)(0x80 | ((cp >> 6) & 63)); *(*o)++ = (char)(0x80 | (cp & 63)); }
}

static char *parse_string(JP *j) {
  if (*j->p != '"') { j->err = 1; return NULL; }
  j->p++;
  const char *start = j->p;
  size_t n = 0;
  while (start[n] && start[n] != '"') { if (start[n] == '\\' && start[n + 1]) n++; n++; }
  char *out = malloc(n * 3 + 1), *o = out;   /* worst case for \u escapes */
  if (!out) { j->err = 1; return NULL; }
  while (*j->p && *j->p != '"') {
    char c = *j->p++;
    if (c != '\\') { *o++ = c; continue; }
    c = *j->p++;
    switch (c) {
      case 'n': *o++ = '\n'; break;
      case 't': *o++ = '\t'; break;
      case 'r': break;
      case 'b': case 'f': break;
      case 'u': {
        unsigned cp = 0;
        for (int i = 0; i < 4 && isxdigit((unsigned char)*j->p); i++, j->p++)
          cp = cp * 16 + (unsigned)(isdigit((unsigned char)*j->p) ? *j->p - '0' : (tolower((unsigned char)*j->p) - 'a' + 10));
        if (cp >= 0xD800 && cp <= 0xDFFF) {        /* surrogate pair: skip */
          if (j->p[0] == '\\' && j->p[1] == 'u') j->p += 6;
          *o++ = '?';
        } else put_utf8(&o, cp);
        break;
      }
      case 0: j->p--; break;
      default: *o++ = c;
    }
  }
  if (*j->p != '"') { free(out); j->err = 1; return NULL; }
  j->p++;
  *o = 0;
  return out;
}

static void parse_value(JP *j, JVal *v);

static void add_kid(JVal *v, JVal *k) {
  if ((v->nk & (v->nk - 1)) == 0) {          /* grow at powers of two */
    int cap = v->nk ? v->nk * 2 : 4;
    JVal *nk = realloc(v->kids, cap * sizeof(JVal));
    if (!nk) return;
    v->kids = nk;
  }
  v->kids[v->nk++] = *k;
}

static void parse_value(JP *j, JVal *v) {
  memset(v, 0, sizeof(*v));
  if (++j->depth > 64) { j->err = 1; return; }
  ws(j);
  char c = *j->p;
  if (c == '{' || c == '[') {
    char end = (c == '{') ? '}' : ']';
    v->t = (c == '{') ? J_OBJ : J_ARR;
    j->p++;
    ws(j);
    if (*j->p == end) { j->p++; j->depth--; return; }
    while (!j->err) {
      JVal kid;
      char *key = NULL;
      ws(j);
      if (v->t == J_OBJ) {
        key = parse_string(j);
        if (j->err) break;
        ws(j);
        if (*j->p != ':') { free(key); j->err = 1; break; }
        j->p++;
      }
      parse_value(j, &kid);
      kid.key = key;
      add_kid(v, &kid);
      if (j->err) break;
      ws(j);
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == end) { j->p++; break; }
      j->err = 1;
    }
  } else if (c == '"') {
    v->t = J_STR;
    v->s = parse_string(j);
  } else if (c == 't' && !strncmp(j->p, "true", 4)) { v->t = J_BOOL; v->n = 1; j->p += 4; }
  else if (c == 'f' && !strncmp(j->p, "false", 5)) { v->t = J_BOOL; j->p += 5; }
  else if (c == 'n' && !strncmp(j->p, "null", 4)) { v->t = J_NULL; j->p += 4; }
  else if (c == '-' || isdigit((unsigned char)c)) {
    char *e;
    v->t = J_NUM;
    v->n = strtod(j->p, &e);
    j->p = e;
  } else j->err = 1;
  j->depth--;
}

static void free_inner(JVal *v) {
  for (int i = 0; i < v->nk; i++) free_inner(&v->kids[i]);
  free(v->kids);
  free(v->key);
  free(v->s);
}

void json_free(JVal *v) { if (v) { free_inner(v); free(v); } }

JVal *json_parse(const char *text) {
  JP j = { text, 0, 0 };
  JVal *v = malloc(sizeof(JVal));
  if (!v) return NULL;
  parse_value(&j, v);
  if (j.err) { json_free(v); return NULL; }
  return v;
}

JVal *jget(const JVal *o, const char *key) {
  if (!o || o->t != J_OBJ) return NULL;
  for (int i = 0; i < o->nk; i++) if (o->kids[i].key && !strcmp(o->kids[i].key, key)) return &o->kids[i];
  return NULL;
}
const char *jstr(const JVal *o, const char *key) {
  JVal *v = jget(o, key);
  return (v && v->t == J_STR && v->s) ? v->s : "";
}
double jnum(const JVal *o, const char *key) {
  JVal *v = jget(o, key);
  return (v && (v->t == J_NUM || v->t == J_BOOL)) ? v->n : 0;
}
int jbool(const JVal *o, const char *key) { return jnum(o, key) != 0; }
