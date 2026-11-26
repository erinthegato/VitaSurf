#ifndef NET_H
#define NET_H
#include <stddef.h>

int net_init(void);
/* Returns 0 on success. *out is malloc'd (caller frees), NUL-terminated. */
int net_fetch(const char *url, char **out, size_t *len,
              char *final_url, size_t final_sz, char *err, size_t err_sz);
#endif
