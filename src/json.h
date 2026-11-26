#ifndef JSON_H
#define JSON_H

/* Tiny JSON reader, enough for GitHub's API. */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
  JType t;
  char *key;            /* set when this value is inside an object */
  char *s;              /* string value */
  double n;             /* number value (bools: 0 or 1) */
  struct JVal *kids;    /* array items / object members */
  int nk;
} JVal;

JVal *json_parse(const char *text);   /* NULL on error */
void json_free(JVal *v);

JVal *jget(const JVal *obj, const char *key);
const char *jstr(const JVal *obj, const char *key);   /* "" if missing */
double jnum(const JVal *obj, const char *key);         /* 0 if missing */
int jbool(const JVal *obj, const char *key);
#endif
