#ifndef NET_H
#define NET_H
#include <stddef.h>

typedef struct {
  const char *url;
  const char *accept;   /* Accept header, or NULL */
  const char *token;    /* GitHub token; only sent to api.github.com */
} NetReq;

typedef struct {
  char *body;           /* malloc'd, NUL-terminated; caller frees */
  size_t len;
  long status;
  char final_url[1024];
  char ctype[128];
} NetResp;

/* Return non-zero from the callback to cancel. */
typedef int (*NetProgress)(void *user, double done, double total);

int net_init(void);
int net_get(const NetReq *r, NetResp *out, char *err, size_t err_sz);
/* Web pages only (rejects images, zips, ...). *out is malloc'd. */
int net_fetch(const char *url, char **out, size_t *len,
              char *final_url, size_t final_sz, char *err, size_t err_sz);
int net_download(const char *url, const char *path, const char *accept, const char *token,
                 NetProgress progress, void *user, char *err, size_t err_sz);
#endif
