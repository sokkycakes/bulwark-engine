/* Bulwark engine - HTTP downloads in the background. ISC licence, see LICENSE. */
#ifndef BW_HTTP_H
#define BW_HTTP_H

#include <stdbool.h>
#include <stddef.h>

enum { HTTP_RUNNING, HTTP_DONE, HTTP_FAILED };

typedef struct http_s http_t;

/* starts fetching url, into savepath (an OS path) or into memory when NULL */
http_t *HTTP_Get(const char *url, const char *savepath);
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
