#ifndef ABSVITA_URL_H
#define ABSVITA_URL_H
#include <stddef.h>

/* "http://h:p/base" + "/api/x" -> "http://h:p/base/api/x" (no doubled slash). */
int url_join(const char *base, const char *path, char *out, size_t n);
/* Accepts http:// or https://host[:port][/path]. Rejects other schemes and junk. 1 when valid. */
int url_valid(const char *s);
/* Trims whitespace and trailing slashes, adds "http://" when no scheme. */
void url_normalize(char *s, size_t n);
/* Percent-encodes a query value. */
int url_encode(const char *in, char *out, size_t n);

#endif
