/* Bulwark engine - HTTP downloads in the background. ISC licence, see LICENSE. */
#ifndef BW_HTTP_H
#define BW_HTTP_H

#include <stdbool.h>
#include <stddef.h>

enum { HTTP_RUNNING, HTTP_DONE, HTTP_FAILED };

typedef struct http_s http_t;

/* starts fetching url, into savepath (an OS path) or into memory when NULL */
http_t *HTTP_Get(const char *url, const char *savepath);
/* an API request, answered into memory: method is "GET", "POST" and so on,
 * body (len bytes, may be NULL) is sent with ctype as its Content-Type, and
 * headers holds extra "Name: value\r\n" lines or is NULL. Any status the
 * server answers with counts as done, with its body kept, and redirects are
 * not followed, so the extra headers never reach another host. */
http_t *HTTP_Request(const char *method, const char *url, const char *ctype, const void *body, size_t len,
                     const char *headers);
int HTTP_State(const http_t *h);
int HTTP_Code(const http_t *h);
const char *HTTP_Error(const http_t *h);
const char *HTTP_URL(const http_t *h);
size_t HTTP_Progress(const http_t *h, size_t *total); /* bytes so far; *total 0 when unknown */
const char *HTTP_Body(const http_t *h, size_t *len);  /* memory downloads */
void HTTP_Cancel(http_t *h);
void HTTP_Free(http_t *h); /* cancels it if still running */
bool HTTP_HaveTLS(void);   /* https:// works (libcurl is installed) */

#endif
