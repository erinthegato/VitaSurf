#include "net.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#define MAX_BYTES (3 * 1024 * 1024)
#define NET_POOL  (1024 * 1024)

static char net_pool[NET_POOL];

struct mem { char *d; size_t n, cap; };

static size_t on_data(void *p, size_t sz, size_t cnt, void *user) {
  struct mem *m = user;
  size_t t = sz * cnt;
  if (m->n + t > MAX_BYTES) return 0;           /* too big: stop, keep what we have */
  if (m->n + t + 1 > m->cap) {
    size_t nc = m->cap ? m->cap * 2 : 65536;
    while (nc < m->n + t + 1) nc *= 2;
    char *nd = realloc(m->d, nc);
    if (!nd) return 0;
    m->d = nd; m->cap = nc;
  }
  memcpy(m->d + m->n, p, t);
  m->n += t;
  m->d[m->n] = 0;
  return t;
}

int net_init(void) {
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  SceNetInitParam p;
  p.memory = net_pool;
  p.size = sizeof(net_pool);
  p.flags = 0;
  sceNetInit(&p);
  sceNetCtlInit();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  return 0;
}

int net_fetch(const char *url, char **out, size_t *len,
              char *final_url, size_t final_sz, char *err, size_t err_sz) {
  struct mem m = {0};
  char upgraded[1100];
  *out = NULL; *len = 0;
  if (strncmp(url, "http://", 7) == 0) {          /* always upgrade to https */
    snprintf(upgraded, sizeof(upgraded), "https://%s", url + 7);
    url = upgraded;
  }
  CURL *c = curl_easy_init();
  if (!c) { snprintf(err, err_sz, "Could not start network"); return -1; }

  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(c, CURLOPT_USERAGENT,
    "Mozilla/5.0 (PlayStation Vita 3.74) AppleWebKit/537.73 (KHTML, like Gecko) VitaSurf/1.0");
  /* HTTPS only, TLS 1.2 or newer, certificates verified against the
     CA bundle packed into the VPK (app0:cacert.pem). */
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(c, CURLOPT_CAINFO, "app0:cacert.pem");
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && m.n > 0)) {
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE)
      snprintf(err, err_sz, "Secure connection failed (certificate check). Make sure the Vita clock is correct. (%s)", curl_easy_strerror(rc));
    else
      snprintf(err, err_sz, "%s", curl_easy_strerror(rc));
    curl_easy_cleanup(c);
    free(m.d);
    return -1;
  }

  char *eff = NULL, *ctype = NULL;
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
  curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  snprintf(final_url, final_sz, "%s", eff ? eff : url);

  if (ctype && !strstr(ctype, "text") && !strstr(ctype, "html") && !strstr(ctype, "xml")) {
    snprintf(err, err_sz, "Unsupported content type: %s", ctype);
    curl_easy_cleanup(c);
    free(m.d);
    return -1;
  }
  if (code >= 400 && m.n == 0) {
    snprintf(err, err_sz, "Server returned HTTP %ld", code);
    curl_easy_cleanup(c);
    free(m.d);
    return -1;
  }
  curl_easy_cleanup(c);
  if (!m.d) { m.d = calloc(1, 1); }
  *out = m.d;
  *len = m.n;
  return 0;
}
