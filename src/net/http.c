/* Bulwark engine - HTTP downloads on a worker thread each. http:// URLs
 * are fetched with a small HTTP/1.1 client of our own (redirects, chunked
 * bodies, the http_proxy setting); https:// ones need the system's libcurl,
 * loaded when first wanted, since TLS is out of the engine's scope.
 * ISC licence, see LICENSE. */
#include "http.h"
#include "../common/common.h"

#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

struct http_s {
	char url[1024];
	char *savepath; /* NULL: keep the body in memory */
	pthread_t thread;
	volatile int state; /* HTTP_RUNNING, HTTP_DONE, HTTP_FAILED */
	volatile bool cancel;
	volatile size_t got, total;
	int code;
	char error[256];
	strbuf_t body;
	FILE *f;
};

/* --- the body sink -------------------------------------------------------- */

static bool sink(http_t *h, const void *data, size_t len)
{
	if (h->cancel)
		return false;
	if (h->f) {
		if (fwrite(data, 1, len, h->f) != len) {
			snprintf(h->error, sizeof(h->error), "can't write %s", h->savepath);
			return false;
		}
	} else {
		SB_Append(&h->body, data, len);
	}
	h->got += len;
	return true;
}

/* --- libcurl, when it's there ---------------------------------------------------- */

/* the few entry points used, by their documented values */
#define BW_CURLOPT_WRITEDATA 10001
#define BW_CURLOPT_URL 10002
#define BW_CURLOPT_USERAGENT 10018
#define BW_CURLOPT_ERRORBUFFER 10010
#define BW_CURLOPT_WRITEFUNCTION 20011
#define BW_CURLOPT_FOLLOWLOCATION 52
#define BW_CURLOPT_NOSIGNAL 99
#define BW_CURLOPT_XFERINFOFUNCTION 20219
#define BW_CURLOPT_XFERINFODATA 10057
#define BW_CURLOPT_NOPROGRESS 43
#define BW_CURLINFO_RESPONSE_CODE 0x200002
#define BW_CURL_GLOBAL_ALL 3

static struct {
	bool tried, ok;
	void *(*easy_init)(void);
	int (*easy_setopt)(void *, int, ...);
	int (*easy_perform)(void *);
	void (*easy_cleanup)(void *);
	int (*easy_getinfo)(void *, int, ...);
	const char *(*easy_strerror)(int);
	int (*global_init)(long);
} curl;
static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;

static bool load_curl(void)
{
	static const char *const names[] = {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl.dylib",
	                                    "libcurl.4.dylib", NULL};
	void *lib = NULL;
	pthread_mutex_lock(&curl_lock);
	if (curl.tried) {
		pthread_mutex_unlock(&curl_lock);
		return curl.ok;
	}
	curl.tried = true;
	for (int i = 0; names[i] && !lib; i++)
		lib = dlopen(names[i], RTLD_NOW);
	if (lib) {
		*(void **)&curl.easy_init = dlsym(lib, "curl_easy_init");
		*(void **)&curl.easy_setopt = dlsym(lib, "curl_easy_setopt");
		*(void **)&curl.easy_perform = dlsym(lib, "curl_easy_perform");
		*(void **)&curl.easy_cleanup = dlsym(lib, "curl_easy_cleanup");
		*(void **)&curl.easy_getinfo = dlsym(lib, "curl_easy_getinfo");
		*(void **)&curl.easy_strerror = dlsym(lib, "curl_easy_strerror");
		*(void **)&curl.global_init = dlsym(lib, "curl_global_init");
		curl.ok = curl.easy_init && curl.easy_setopt && curl.easy_perform && curl.easy_cleanup && curl.easy_getinfo &&
		          curl.global_init;
		if (curl.ok)
			curl.global_init(BW_CURL_GLOBAL_ALL);
	}
	pthread_mutex_unlock(&curl_lock);
	return curl.ok;
}

bool HTTP_HaveTLS(void) { return load_curl(); }

static size_t curl_write(char *data, size_t size, size_t n, void *ctx)
{
	return sink(ctx, data, size * n) ? size * n : 0;
}

static int curl_progress(void *ctx, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow)
{
	http_t *h = ctx;
	(void)dlnow, (void)ultotal, (void)ulnow;
	if (dltotal > 0)
		h->total = (size_t)dltotal;
	return h->cancel;
}

static bool fetch_curl(http_t *h)
{
	char err[256] = "";
	void *c = curl.easy_init();
	int rc;
	long code = 0;
	if (!c) {
		Str_Copy(h->error, "libcurl failed to start", sizeof(h->error));
		return false;
	}
	curl.easy_setopt(c, BW_CURLOPT_URL, h->url);
	curl.easy_setopt(c, BW_CURLOPT_WRITEFUNCTION, curl_write);
	curl.easy_setopt(c, BW_CURLOPT_WRITEDATA, h);
	curl.easy_setopt(c, BW_CURLOPT_FOLLOWLOCATION, 1L);
	curl.easy_setopt(c, BW_CURLOPT_NOSIGNAL, 1L);
	curl.easy_setopt(c, BW_CURLOPT_USERAGENT, "Bulwark/" BULWARK_VERSION);
	curl.easy_setopt(c, BW_CURLOPT_ERRORBUFFER, err);
	curl.easy_setopt(c, BW_CURLOPT_NOPROGRESS, 0L);
	curl.easy_setopt(c, BW_CURLOPT_XFERINFOFUNCTION, curl_progress);
	curl.easy_setopt(c, BW_CURLOPT_XFERINFODATA, h);
	rc = curl.easy_perform(c);
	curl.easy_getinfo(c, BW_CURLINFO_RESPONSE_CODE, &code);
	curl.easy_cleanup(c);
	h->code = (int)code;
	if (rc) {
		if (!h->error[0])
			Str_Copy(h->error, err[0] ? err : curl.easy_strerror ? curl.easy_strerror(rc) : "transfer failed",
			         sizeof(h->error));
		return false;
	}
	return true;
}

/* --- plain HTTP -------------------------------------------------------------------- */

static bool split_url(const char *url, char *host, size_t hsize, int *port, char *path, size_t psize)
{
	const char *p = url, *slash, *colon;
	if (!strncasecmp(p, "http://", 7))
		p += 7;
	else
		return false;
	slash = strchr(p, '/');
	if (!slash)
		slash = p + strlen(p);
	Str_Copy(host, p, BW_MIN(hsize, (size_t)(slash - p) + 1));
	*port = 80;
	if ((colon = strchr(host, ':'))) {
		*port = atoi(colon + 1);
		host[colon - host] = 0;
	}
	Str_Copy(path, *slash ? slash : "/", psize);
	return host[0] != 0;
}

static int connect_to(const char *host, int port)
{
	struct addrinfo hints, *res, *ai;
	char portstr[16];
	int s = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res))
		return -1;
	for (ai = res; ai && s < 0; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s >= 0 && connect(s, ai->ai_addr, ai->ai_addrlen) < 0) {
			close(s);
			s = -1;
		}
	}
	freeaddrinfo(res);
	return s;
}

static bool send_all(int s, const char *data, size_t len)
{
	while (len) {
		ssize_t n = send(s, data, len, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		data += n;
		len -= (size_t)n;
	}
	return true;
}

/* reads from s into buf until a blank line ends the headers; leftover body bytes stay in buf */
static bool read_headers(int s, strbuf_t *buf, size_t *headlen)
{
	char tmp[4096];
	for (;;) {
		char *end;
		ssize_t n;
		SB_Append(buf, "", 1);
		buf->len--;
		if ((end = strstr(buf->data, "\r\n\r\n"))) {
			*headlen = (size_t)(end - buf->data) + 4;
			return true;
		}
		if (buf->len > 65536 || (n = recv(s, tmp, sizeof(tmp), 0)) <= 0)
			return false;
		SB_Append(buf, tmp, (size_t)n);
	}
}

static const char *header_value(const char *head, const char *key, char *out, size_t size)
{
	size_t kl = strlen(key);
	out[0] = 0;
	for (const char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
		const char *v = l + 2;
		if (!strncasecmp(v, key, kl) && v[kl] == ':') {
			size_t n;
			v += kl + 1;
			while (*v == ' ')
				v++;
			n = strcspn(v, "\r\n");
			Str_Copy(out, v, BW_MIN(size, n + 1));
			return out;
		}
	}
	return NULL;
}

/* one request; *redirect gets a Location to follow */
static bool fetch_once(http_t *h, const char *url, char *redirect, size_t rsize)
{
	char host[256], path[1024], req[2048], v[1024], proxyhost[256], proxypath[16];
	const char *proxy = getenv("http_proxy");
	int port, s, pport;
	strbuf_t buf = {0};
	size_t headlen, at;
	bool chunked, ok = false;
	long long length = -1;
	redirect[0] = 0;
	if (!split_url(url, host, sizeof(host), &port, path, sizeof(path))) {
		snprintf(h->error, sizeof(h->error), "not an http:// address: %s", url);
		return false;
	}
	if (!proxy)
		proxy = getenv("HTTP_PROXY");
	if (proxy && proxy[0] && split_url(proxy, proxyhost, sizeof(proxyhost), &pport, proxypath, sizeof(proxypath)))
		s = connect_to(proxyhost, pport);
	else {
		proxy = NULL;
		s = connect_to(host, port);
	}
	if (s < 0) {
		snprintf(h->error, sizeof(h->error), "can't connect to %.200s", proxy ? proxy : host);
		return false;
	}
	snprintf(req, sizeof(req),
	         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Bulwark/" BULWARK_VERSION "\r\nConnection: close\r\n\r\n",
	         proxy ? url : path, host);
	if (!send_all(s, req, strlen(req)) || !read_headers(s, &buf, &headlen)) {
		snprintf(h->error, sizeof(h->error), "no answer from %.200s", host);
		goto done;
	}
	buf.data[headlen - 2] = 0;
	h->code = atoi(strchr(buf.data, ' ') ? strchr(buf.data, ' ') + 1 : "0");
	if (h->code >= 300 && h->code < 400 && header_value(buf.data, "Location", v, sizeof(v))) {
		Str_Copy(redirect, v, rsize);
		ok = true;
		goto done;
	}
	if (h->code != 200) {
		snprintf(h->error, sizeof(h->error), "HTTP %d", h->code);
		goto done;
	}
	chunked = header_value(buf.data, "Transfer-Encoding", v, sizeof(v)) && strstr(v, "chunked");
	if (header_value(buf.data, "Content-Length", v, sizeof(v)))
		length = atoll(v);
	if (length >= 0)
		h->total = (size_t)length;
	/* the body: what came with the headers, then the rest */
	{
		strbuf_t body = {0};
		char tmp[16384];
		ssize_t n;
		SB_Append(&body, buf.data + headlen, buf.len - headlen);
		if (!chunked) {
			if (!sink(h, body.data, body.len)) {
				SB_Free(&body);
				goto done;
			}
			while ((length < 0 || (long long)h->got < length) && (n = recv(s, tmp, sizeof(tmp), 0)) > 0)
				if (!sink(h, tmp, (size_t)n)) {
					SB_Free(&body);
					goto done;
				}
			ok = length < 0 || (long long)h->got >= length;
			if (!ok)
				Str_Copy(h->error, "the connection closed early", sizeof(h->error));
			SB_Free(&body);
			goto done;
		}
		/* chunked: size line, data, CRLF ... a zero size ends it */
		at = 0;
		for (;;) {
			char *line_end;
			unsigned long size;
			SB_Append(&body, "", 1);
			body.len--;
			while (!(line_end = strstr(body.data + at, "\r\n"))) {
				if ((n = recv(s, tmp, sizeof(tmp), 0)) <= 0)
					break;
				SB_Append(&body, tmp, (size_t)n);
				SB_Append(&body, "", 1);
				body.len--;
			}
			if (!line_end)
				break;
			size = strtoul(body.data + at, NULL, 16);
			at = (size_t)(line_end - body.data) + 2;
			if (!size) {
				ok = true;
				break;
			}
			while (body.len - at < size + 2) {
				if ((n = recv(s, tmp, sizeof(tmp), 0)) <= 0)
					break;
				SB_Append(&body, tmp, (size_t)n);
			}
			if (body.len - at < size)
				break;
			if (!sink(h, body.data + at, size))
				break;
			at += size + 2;
			if (at > (1u << 20)) {
				memmove(body.data, body.data + at, body.len - at);
				body.len -= at;
				at = 0;
			}
		}
		if (!ok && !h->error[0])
			Str_Copy(h->error, "the connection closed early", sizeof(h->error));
		SB_Free(&body);
	}
done:
	SB_Free(&buf);
	close(s);
	return ok;
}

static bool fetch_plain(http_t *h)
{
	char url[1024], next[1024];
	Str_Copy(url, h->url, sizeof(url));
	for (int hops = 0; hops < 8; hops++) {
		if (!fetch_once(h, url, next, sizeof(next)))
			return false;
		if (!next[0])
			return true;
		if (!strncasecmp(next, "https://", 8)) {
			/* sent on to TLS */
			Str_Copy(h->url, next, sizeof(h->url));
			if (!load_curl()) {
				Str_Copy(h->error, "the server sent us to https://, which needs libcurl", sizeof(h->error));
				return false;
			}
			return fetch_curl(h);
		}
		Str_Copy(url, next, sizeof(url));
	}
	Str_Copy(h->error, "too many redirects", sizeof(h->error));
	return false;
}

static void *worker(void *ctx)
{
	http_t *h = ctx;
	bool ok;
	if (h->savepath && !(h->f = fopen(h->savepath, "wb"))) {
		snprintf(h->error, sizeof(h->error), "can't write %s", h->savepath);
		h->state = HTTP_FAILED;
		return NULL;
	}
	if (!strncasecmp(h->url, "https://", 8) || !strncasecmp(h->url, "ftp://", 6)) {
		if (!load_curl()) {
			Str_Copy(h->error, "https:// needs libcurl, which isn't installed", sizeof(h->error));
			ok = false;
		} else
			ok = fetch_curl(h);
	} else if (load_curl() && getenv("BW_HTTP_USE_CURL")) {
		ok = fetch_curl(h);
	} else {
		ok = fetch_plain(h);
	}
	if (h->f) {
		fclose(h->f);
		h->f = NULL;
		if (!ok)
			remove(h->savepath);
	}
	if (h->cancel && !h->error[0])
		Str_Copy(h->error, "cancelled", sizeof(h->error));
	h->state = ok && !h->cancel ? HTTP_DONE : HTTP_FAILED;
	return NULL;
}

http_t *HTTP_Get(const char *url, const char *savepath)
{
	http_t *h = Mem_Alloc(sizeof(*h));
	Str_Copy(h->url, url, sizeof(h->url));
	h->savepath = savepath ? Mem_Strdup(savepath) : NULL;
	h->state = HTTP_RUNNING;
	if (pthread_create(&h->thread, NULL, worker, h)) {
		Str_Copy(h->error, "can't start a download thread", sizeof(h->error));
		h->state = HTTP_FAILED;
		h->thread = 0;
	}
	return h;
}

int HTTP_State(const http_t *h) { return h->state; }
int HTTP_Code(const http_t *h) { return h->code; }
const char *HTTP_Error(const http_t *h) { return h->error; }
const char *HTTP_URL(const http_t *h) { return h->url; }

size_t HTTP_Progress(const http_t *h, size_t *total)
{
	if (total)
		*total = h->total;
	return h->got;
}

const char *HTTP_Body(const http_t *h, size_t *len)
{
	if (len)
		*len = h->body.len;
	return h->body.data ? h->body.data : "";
}

void HTTP_Cancel(http_t *h) { h->cancel = true; }

void HTTP_Free(http_t *h)
{
	if (!h)
		return;
	h->cancel = true;
	if (h->thread)
		pthread_join(h->thread, NULL);
	SB_Free(&h->body);
	Mem_Free(h->savepath);
	Mem_Free(h);
}
