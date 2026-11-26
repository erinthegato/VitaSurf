#include "net.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#define MAX_BYTES (3 * 1024 * 1024)
#define NET_POOL  (1024 * 1024)

static char net_pool[NET_POOL];

/* Trusted certificates, loaded once from the VPK (app0:cacert.pem).
   We parse them one at a time ourselves: OpenSSL's bulk loader gives up on
   the whole file if a single certificate can't be read on the Vita. */
static X509_STORE *ca_store;
static int ca_count, ca_total;

static char *read_file(const char *path, long *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *d = (n > 0) ? malloc(n + 1) : NULL;
  if (d && fread(d, 1, n, f) != (size_t)n) { free(d); d = NULL; }
  fclose(f);
  if (d) { d[n] = 0; *len = n; }
  return d;
}

static int load_ca(void) {
  if (ca_store) return 0;
  long len = 0;
  char *pem = read_file("app0:cacert.pem", &len);
  if (!pem) pem = read_file("app0:/cacert.pem", &len);
  if (!pem) return -1;

  X509_STORE *store = X509_STORE_new();
  const char *BEGIN = "-----BEGIN CERTIFICATE-----";
  const char *END = "-----END CERTIFICATE-----";
  char *p = pem;
  while ((p = strstr(p, BEGIN))) {
    char *e = strstr(p, END);
    if (!e) break;
    e += strlen(END);
    ca_total++;
    BIO *b = BIO_new_mem_buf(p, (int)(e - p));
    X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    if (x) {
      if (X509_STORE_add_cert(store, x) == 1) ca_count++;
      X509_free(x);
    }
    if (b) BIO_free(b);
    p = e;
  }
  ERR_clear_error();
  free(pem);
  if (ca_count == 0) { X509_STORE_free(store); return -2; }
  ca_store = store;
  return 0;
}

/* Called by curl for each new connection: give it our certificate store. */
static CURLcode ssl_ctx_cb(CURL *c, void *ssl_ctx, void *user) {
  (void)c; (void)user;
  X509_STORE_up_ref(ca_store);
  SSL_CTX_set_cert_store((SSL_CTX *)ssl_ctx, ca_store);
  return CURLE_OK;
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
  int ca = load_ca();
  if (ca == -1) {
    snprintf(err, err_sz, "Certificate file missing from the app (cacert.pem). Rebuild the VPK.");
    return -1;
  }
  if (ca == -2) {
    snprintf(err, err_sz, "Could not read any of the %d bundled certificates.", ca_total);
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
     Vita), and hand it our own certificate store instead. */
  curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
  curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
  curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
  char detail[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, detail);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && m.n > 0)) {
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE)
      snprintf(err, err_sz, "Secure connection failed (certificate check, %d of %d certificates loaded). %s",
               ca_count, ca_total, detail[0] ? detail : curl_easy_strerror(rc));
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
