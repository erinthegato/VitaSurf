#ifndef GITHUB_H
#define GITHUB_H
#include <stddef.h>
#include "net.h"

/* GitHub mode: "gh:" addresses are turned into simple HTML pages using
   GitHub's API, because github.com itself needs a full JavaScript browser.

   gh:                         GitHub home
   gh:search?q=TEXT            repository search
   gh:user/NAME                a user's or organisation's repositories
   gh:me                       your repositories (signed in)
   gh:repo/OWNER/REPO          repository overview + README
   gh:files/OWNER/REPO/PATH    folder listing    (?ref=BRANCH optional)
   gh:file/OWNER/REPO/PATH     file contents     (?ref=BRANCH optional)
   gh:releases/OWNER/REPO      releases and downloads
   ghdl:OWNER|REPO|ID|TAG|NAME a release file to download

   Actions handled by the app itself: gh:signin, gh:signout, gh:searchbox, gh:openbox */

int gh_is(const char *url);
int gh_is_download(const char *url);
/* github.com/... address -> gh: address. Returns 1 if converted. */
int gh_from_web(const char *url, char *out, size_t out_sz);
/* Builds the page. Returns malloc'd HTML; base gets the address relative links use. */
char *gh_render(const char *url, char *base, size_t base_sz);

void gh_load_token(void);
const char *gh_token(void);
void gh_set_token(const char *tok);

const char *gh_download_dir(void);
/* Downloads a ghdl: link. saved_path gets where the file went. */
int gh_download(const char *url, NetProgress progress, void *user,
                char *saved_path, size_t saved_sz, char *err, size_t err_sz);
#endif
