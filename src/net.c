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

/* CA certificates, read once from the VPK into memory. We read the file
   ourselves and hand curl the bytes, because OpenSSL can't open app0:
   paths on its own. */
static struct curl_blob ca_blob;

static int load_ca(void) {
  if (ca_blob.data) return 0;
  const char *paths[] = {"app0:cacert.pem", "app0:/cacert.pem"};
  for (int i = 0; i < 2; i++) {
    FILE *f = fopen(paths[i], "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = (n > 0) ? malloc(n) : NULL;
    if (d && fread(d, 1, n, f) == (size_t)n) {
      fclose(f);
      ca_blob.data = d;
      ca_blob.len = (size_t)n;
      ca_blob.flags = CURL_BLOB_NOCOPY;
      return 0;
    }
    free(d);
    fclose(f);
  }
  return -1;
}

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
  if (load_ca() != 0) {
    snprintf(err, err_sz, "Certificate file missing from the app (cacert.pem). Rebuild the VPK.");
    return -1;
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
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  /* Clear curl's built-in CA file/folder paths (they don't exist on the
     Vita and make the certificate check fail), then use our in-memory copy. */
  curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
  curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
  curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca_blob);
  char detail[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, detail);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && m.n > 0)) {
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE)
      snprintf(err, err_sz, "Secure connection failed (certificate check). %s", detail[0] ? detail : curl_easy_strerror(rc));
    else
      snprintf(err, err_sz, "%s", detail[0] ? detail : curl_easy_strerror(rc));
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
