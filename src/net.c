#include "net.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <psp2/kernel/rng.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>

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

/* Trusted certificates.
   Keeping ~150 parsed certificates alive uses up the Vita's thread locks
   (OpenSSL gives every certificate its own lock), and then OpenSSL can't
   create anything else ("X509_STORE_new: malloc failure"). So we keep the
   certificates as text, remember each one's subject hash, and only turn the
   one or two a site actually needs into OpenSSL objects, during that
   connection. */
typedef struct { unsigned long hash; const char *pem; int len; } CaEntry;
static char *ca_pem;          /* the whole cacert.pem file */
static CaEntry *ca_list;
static int ca_count, ca_total;
static X509_LOOKUP_METHOD *ca_method;

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

static X509 *parse_pem(const char *pem, int len) {
  BIO *b = BIO_new_mem_buf(pem, len);
  if (!b) return NULL;
  X509 *x = PEM_read_bio_X509(b, NULL, NULL, NULL);
  BIO_free(b);
  return x;
}

/* OpenSSL asks: "do you have a trusted certificate with this subject?" */
static int ca_lookup(X509_LOOKUP *ctx, X509_LOOKUP_TYPE type, X509_NAME *name, X509_OBJECT *ret) {
  if (type != X509_LU_X509 || !name) return 0;
  unsigned long h = X509_NAME_hash(name);
  X509_STORE *store = X509_LOOKUP_get_store(ctx);
  int found = 0;
  for (int i = 0; i < ca_count; i++) {
    if (ca_list[i].hash != h) continue;
    X509 *x = parse_pem(ca_list[i].pem, ca_list[i].len);
    if (!x) continue;
    if (X509_NAME_cmp(X509_get_subject_name(x), name) == 0) {
      X509_STORE_add_cert(store, x);          /* may already be there; that's fine */
      if (!found) { X509_OBJECT_set1_X509(ret, x); found = 1; }
    }
    X509_free(x);
  }
  ERR_clear_error();
  return found;
}

static int load_ca(void) {
  if (ca_method) return 0;
  long len = 0;
  if (!ca_pem) ca_pem = read_file("app0:cacert.pem", &len);
  if (!ca_pem) ca_pem = read_file("app0:/cacert.pem", &len);
  if (!ca_pem) return -1;

  const char *BEGIN = "-----BEGIN CERTIFICATE-----";
  const char *END = "-----END CERTIFICATE-----";
  int cap = 0;
  ca_count = ca_total = 0;
  for (char *p = ca_pem; (p = strstr(p, BEGIN)); ) {
    char *e = strstr(p, END);
    if (!e) break;
    e += strlen(END);
    ca_total++;
    X509 *x = parse_pem(p, (int)(e - p));       /* parse once to get the hash, then free */
    if (x) {
      if (ca_count == cap) {
        cap = cap ? cap * 2 : 128;
        CaEntry *nl = realloc(ca_list, cap * sizeof(CaEntry));
        if (!nl) { X509_free(x); break; }
        ca_list = nl;
      }
      ca_list[ca_count].hash = X509_NAME_hash(X509_get_subject_name(x));
      ca_list[ca_count].pem = p;
      ca_list[ca_count].len = (int)(e - p);
      ca_count++;
      X509_free(x);
    }
    p = e;
  }
  ERR_clear_error();
  if (ca_count == 0) return -2;

  ca_method = X509_LOOKUP_meth_new("VitaSurf CA list");
  if (!ca_method) return -2;
  /* cast: the name argument is const in OpenSSL 3 but not in 1.1.1 */
  X509_LOOKUP_meth_set_get_by_subject(ca_method, (X509_LOOKUP_get_by_subject_fn)ca_lookup);
  return 0;
}

/* Called by curl for each new connection: plug our lookup into its store. */
static CURLcode ssl_ctx_cb(CURL *c, void *ssl_ctx, void *user) {
  (void)c; (void)user;
  X509_STORE *store = SSL_CTX_get_cert_store((SSL_CTX *)ssl_ctx);
  if (!store || !X509_STORE_add_lookup(store, ca_method)) return CURLE_SSL_CACERT_BADFILE;
  return CURLE_OK;
}

/* OpenSSL has no source of random numbers on the Vita, so TLS refuses to
   start ("Insufficient randomness"). Feed it the Vita's hardware RNG. */
static void seed_random(int bytes) {
  unsigned char buf[64];
  while (bytes > 0) {
    sceKernelGetRandomNumber(buf, sizeof(buf));
    RAND_add(buf, sizeof(buf), (double)sizeof(buf));
    bytes -= sizeof(buf);
  }
  memset(buf, 0, sizeof(buf));
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
  seed_random(512);
  return 0;
}

/* Common curl setup: HTTPS only, TLS 1.2+, our certificate store. */
static CURL *new_handle(const char *url, char *detail) {
  CURL *c = curl_easy_init();
  if (!c) return NULL;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(c, CURLOPT_USERAGENT,
    "Mozilla/5.0 (PlayStation Vita 3.74) AppleWebKit/537.73 (KHTML, like Gecko) VitaSurf/1.0");
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
  /* Clear curl's built-in CA paths (they don't exist on the Vita) and
     hand it our own certificate store instead. */
  curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
  curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
  curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, detail);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  return c;
}

/* Checks the random generator and certificates are ready. */
static int prepare(char *err, size_t err_sz) {
  seed_random(64);
  if (RAND_status() != 1) {
    snprintf(err, err_sz, "Secure random generator could not be started.");
    return -1;
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
  return 0;
}

static void describe_error(CURLcode rc, const char *detail, char *err, size_t err_sz) {
  if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE)
    snprintf(err, err_sz, "Secure connection failed (certificate check, %d of %d certificates loaded). %s",
             ca_count, ca_total, detail[0] ? detail : curl_easy_strerror(rc));
  else
    snprintf(err, err_sz, "%s", detail[0] ? detail : curl_easy_strerror(rc));
}

int net_get(const NetReq *r, NetResp *out, char *err, size_t err_sz) {
  struct mem m = {0};
  char url[1100], detail[CURL_ERROR_SIZE] = {0};
  memset(out, 0, sizeof(*out));
  if (strncmp(r->url, "http://", 7) == 0) snprintf(url, sizeof(url), "https://%s", r->url + 7);
  else snprintf(url, sizeof(url), "%s", r->url);
  if (prepare(err, err_sz) != 0) return -1;

  CURL *c = new_handle(url, detail);
  if (!c) { snprintf(err, err_sz, "Could not start network"); return -1; }
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

  struct curl_slist *hdr = NULL;
  char line[600];
  if (r->accept) { snprintf(line, sizeof(line), "Accept: %s", r->accept); hdr = curl_slist_append(hdr, line); }
  /* Only ever send the GitHub token to GitHub's API. */
  if (r->token && r->token[0] && strncmp(url, "https://api.github.com/", 23) == 0) {
    snprintf(line, sizeof(line), "Authorization: Bearer %s", r->token);
    hdr = curl_slist_append(hdr, line);
    curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);
  }
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);

  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && m.n > 0)) {
    describe_error(rc, detail, err, err_sz);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdr);
    free(m.d);
    return -1;
  }

  char *eff = NULL, *ctype = NULL;
  curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
  curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
  snprintf(out->final_url, sizeof(out->final_url), "%s", eff ? eff : url);
  snprintf(out->ctype, sizeof(out->ctype), "%s", ctype ? ctype : "");
  curl_easy_cleanup(c);
  curl_slist_free_all(hdr);
  if (!m.d) m.d = calloc(1, 1);
  out->body = m.d;
  out->len = m.n;
  return 0;
}

int net_fetch(const char *url, char **out, size_t *len,
              char *final_url, size_t final_sz, char *err, size_t err_sz) {
  NetReq r = { url, NULL, NULL };
  NetResp resp;
  *out = NULL; *len = 0;
  if (net_get(&r, &resp, err, err_sz) != 0) return -1;
  snprintf(final_url, final_sz, "%s", resp.final_url);
  if (resp.ctype[0] && !strstr(resp.ctype, "text") && !strstr(resp.ctype, "html") && !strstr(resp.ctype, "xml")) {
    snprintf(err, err_sz, "Can't show this kind of file (%s).", resp.ctype);
    free(resp.body);
    return -1;
  }
  if (resp.status >= 400 && resp.len == 0) {
    snprintf(err, err_sz, "Server returned HTTP %ld", resp.status);
    free(resp.body);
    return -1;
  }
  *out = resp.body;
  *len = resp.len;
  return 0;
}

/* ---------- downloads to a file ---------- */

struct dl { NetProgress fn; void *user; };

static int on_progress(void *p, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
  (void)ut; (void)un;
  struct dl *d = p;
  return d->fn ? d->fn(d->user, (double)now, (double)total) : 0;
}

int net_download(const char *url, const char *path, const char *accept, const char *token,
                 NetProgress progress, void *user, char *err, size_t err_sz) {
  char detail[CURL_ERROR_SIZE] = {0};
  if (prepare(err, err_sz) != 0) return -1;
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.part", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) { snprintf(err, err_sz, "Can't write to %s", path); return -1; }

  CURL *c = new_handle(url, detail);
  if (!c) { fclose(f); snprintf(err, err_sz, "Could not start network"); return -1; }
  struct dl d = { progress, user };
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, NULL);   /* default: fwrite */
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);   /* give up if stalled 30s */
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  struct curl_slist *hdr = NULL;
  char line[300];
  if (accept) { snprintf(line, sizeof(line), "Accept: %s", accept); hdr = curl_slist_append(hdr, line); }
  /* Token only goes to api.github.com; curl drops it if GitHub redirects
     the download to another host. */
  if (token && token[0] && strncmp(url, "https://api.github.com/", 23) == 0) {
    snprintf(line, sizeof(line), "Authorization: Bearer %s", token);
    hdr = curl_slist_append(hdr, line);
  }
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  curl_slist_free_all(hdr);
  fclose(f);
  if (rc != CURLE_OK) {
    if (rc == CURLE_ABORTED_BY_CALLBACK) snprintf(err, err_sz, "Download cancelled");
    else describe_error(rc, detail, err, err_sz);
    sceIoRemove(tmp);
    return -1;
  }
  sceIoRemove(path);
  if (sceIoRename(tmp, path) < 0) { snprintf(err, err_sz, "Could not save %s", path); return -1; }
  return 0;
}
