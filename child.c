/*	$Id$ */
/*
 * Copyright (c) 2012, 2014--2016 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#include <arpa/inet.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#if HAVE_MD5
# include <sys/types.h>
# include <md5.h>
#endif
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kcgi.h"
#include "extern.h"

#define MD5Updatec(_ctx, _b, _sz) \
	MD5Update((_ctx), (const u_int8_t *)(_b), (_sz))

/*
 * For handling HTTP multipart forms.
 * This consists of data for a single multipart form entry.
 */
struct	mime {
	char	 *disp; /* content disposition */
	char	 *name; /* name of form entry */
	size_t	  namesz; /* size of "name" string */
	char	 *file; /* whether a file was specified */
	char	 *ctype; /* content type */
	size_t	  ctypepos; /* position of ctype in mimes */
	char	 *xcode; /* encoding type */
	char	 *bound; /* form entry boundary */
};

/*
 * Both CGI and FastCGI use an environment for their HTTP parameters.
 * CGI gets it from the actual environment; FastCGI from a transmitted
 * environment.
 * We use an abstract representation of those key-value pairs here so
 * that we can use the same functions for both.
 */
struct	env {
	char	*key; /* key (e.g., HTTP_HOST) */
	size_t	 keysz;
	char	*val; /* value (e.g., `foo.com') */
	size_t	 valsz;
};

/* 
 * Types of FastCGI requests.
 * Defined in the FastCGI v1.0 spec, section 8.
 */
enum	fcgi_type {
	FCGI_BEGIN_REQUEST = 1,
	FCGI_ABORT_REQUEST = 2,
	FCGI_END_REQUEST = 3,
	FCGI_PARAMS = 4,
	FCGI_STDIN = 5,
	FCGI_STDOUT = 6,
	FCGI_STDERR = 7,
	FCGI_DATA = 8,
	FCGI_GET_VALUES = 9,
	FCGI_GET_VALUES_RESULT = 10,
	FCGI_UNKNOWN_TYPE = 11,
	FCGI__MAX
};

/*
 * The FastCGI `FCGI_Header' header layout.
 * Defined in the FastCGI v1.0 spec, section 8.
 */
struct 	fcgi_hdr {
	uint8_t	 version;
	uint8_t	 type;
	uint16_t requestId;
	uint16_t contentLength;
	uint8_t	 paddingLength;
	uint8_t	 reserved;
};

/*
 * The FastCGI `FCGI_BeginRequestBody' header layout.
 * Defined in the FastCGI v1.0 spec, section 8.
 */
struct	fcgi_bgn {
	uint16_t role;
	uint8_t	 flags;
	uint8_t	 res[5];
};

/*
 * Parameters required to validate fields.
 */
struct	parms {
	int	 		 fd;
	const char *const	*mimes;
	size_t			 mimesz;
	const struct kvalid	*keys;
	size_t			 keysz;
	enum input		 type;
};

const char *const kmethods[KMETHOD__MAX] = {
	"ACL", /* KMETHOD_ACL */
	"CONNECT", /* KMETHOD_CONNECT */
	"COPY", /* KMETHOD_COPY */
	"DELETE", /* KMETHOD_DELETE */
	"GET", /* KMETHOD_GET */
	"HEAD", /* KMETHOD_HEAD */
	"LOCK", /* KMETHOD_LOCK */
	"MKCALENDAR", /* KMETHOD_MKCALENDAR */
	"MKCOL", /* KMETHOD_MKCOL */
	"MOVE", /* KMETHOD_MOVE */
	"OPTIONS", /* KMETHOD_OPTIONS */
	"POST", /* KMETHOD_POST */
	"PROPFIND", /* KMETHOD_PROPFIND */
	"PROPPATCH", /* KMETHOD_PROPPATCH */
	"PUT", /* KMETHOD_PUT */
	"REPORT", /* KMETHOD_REPORT */
	"TRACE", /* KMETHOD_TRACE */
	"UNLOCK", /* KMETHOD_UNLOCK */
};

static	const char *const krequs[KREQU__MAX] = {
	"HTTP_ACCEPT", /* KREQU_ACCEPT */
	"HTTP_ACCEPT_CHARSET", /* KREQU_ACCEPT_CHARSET */
	"HTTP_ACCEPT_ENCODING", /* KREQU_ACCEPT_ENCODING */
	"HTTP_ACCEPT_LANGUAGE", /* KREQU_ACCEPT_LANGUAGE */
	"HTTP_AUTHORIZATION", /* KREQU_AUTHORIZATION */
	"HTTP_DEPTH", /* KREQU_DEPTH */
	"HTTP_FROM", /* KREQU_FROM */
	"HTTP_HOST", /* KREQU_HOST */
	"HTTP_IF", /* KREQU_IF */
	"HTTP_IF_MODIFIED_SINCE", /* KREQU_IF_MODIFIED_SINCE */
	"HTTP_IF_MATCH", /* KREQU_IF_MATCH */
	"HTTP_IF_NONE_MATCH", /* KREQU_IF_NONE_MATCH */
	"HTTP_IF_RANGE", /* KREQU_IF_RANGE */
	"HTTP_IF_UNMODIFIED_SINCE", /* KREQU_IF_UNMODIFIED_SINCE */
	"HTTP_MAX_FORWARDS", /* KREQU_MAX_FORWARDS */
	"HTTP_PROXY_AUTHORIZATION", /* KREQU_PROXY_AUTHORIZATION */
	"HTTP_RANGE", /* KREQU_RANGE */
	"HTTP_REFERER", /* KREQU_REFERER */
	"HTTP_USER_AGENT", /* KREQU_USER_AGENT */
};

static	const char *const kauths[KAUTH_UNKNOWN] = {
	NULL,
	"basic",
	"digest"
};

/*
 * Parse the type/subtype field out of a content-type.
 * The content-type is defined (among other places) in RFC 822, and is
 * either the whole string or up until the ';', which marks the
 * beginning of the parameters.
 */
static size_t
str2ctype(const struct parms *pp, const char *ctype)
{
	size_t		 i, sz;
	const char	*end;
	

	if (NULL == ctype) 
		return(pp->mimesz);

	/* Stop at the content-type parameters. */
	if (NULL == (end = strchr(ctype, ';'))) 
		sz = strlen(ctype);
	else
		sz = end - ctype;

	for (i = 0; i < pp->mimesz; i++) {
		if (sz != strlen(pp->mimes[i]))
			continue;
		if (0 == strncasecmp(pp->mimes[i], ctype, sz))
			return(i);
	}

	return(i);
}

/*
 * Given a parsed field "key" with value "val" of size "valsz" and MIME
 * information "mime", first try to look it up in the array of
 * recognised keys ("pp->keys") and optionally validate.
 * Then output the type, parse status (key, type, etc.), and values read
 * by the parent input() function.
 */
static void
output(const struct parms *pp, char *key, 
	char *val, size_t valsz, struct mime *mime)
{
	size_t	 	 i;
	ptrdiff_t	 diff;
	char		*save;
	struct kpair	 pair;

	memset(&pair, 0, sizeof(struct kpair));

	pair.key = key;
	pair.val = save = val;
	pair.valsz = valsz;
	pair.file = NULL == mime ? NULL : mime->file;
	pair.ctype = NULL == mime ? NULL : mime->ctype;
	pair.xcode = NULL == mime ? NULL : mime->xcode;
	pair.ctypepos = NULL == mime ? pp->mimesz : mime->ctypepos;

	/*
	 * Look up the key name in our key table.
	 * If we find it and it has a validator, then run the validator
	 * and record the output.
	 * Either way, the keypos parameter is going to be the key
	 * identifier or keysz if none is found.
	 */

	for (i = 0; i < pp->keysz; i++) {
		if (strcmp(pp->keys[i].name, pair.key)) 
			continue;
		if (NULL != pp->keys[i].valid)
			pair.state = pp->keys[i].valid(&pair) ?
				KPAIR_VALID : KPAIR_INVALID;
		break;
	}
	pair.keypos = i;

	fullwrite(pp->fd, &pp->type, sizeof(enum input));
	fullwriteword(pp->fd, pair.key);
	fullwrite(pp->fd, &pair.valsz, sizeof(size_t));
	fullwrite(pp->fd, pair.val, pair.valsz);
	fullwrite(pp->fd, &pair.state, sizeof(enum kpairstate));
	fullwrite(pp->fd, &pair.type, sizeof(enum kpairtype));
	fullwrite(pp->fd, &pair.keypos, sizeof(size_t));

	if (KPAIR_VALID == pair.state) 
		switch (pair.type) {
		case (KPAIR_DOUBLE):
			fullwrite(pp->fd, 
				&pair.parsed.d, sizeof(double));
			break;
		case (KPAIR_INTEGER):
			fullwrite(pp->fd, 
				&pair.parsed.i, sizeof(int64_t));
			break;
		case (KPAIR_STRING):
			assert(pair.parsed.s >= pair.val);
			assert(pair.parsed.s <= pair.val + pair.valsz);
			diff = pair.val - pair.parsed.s;
			fullwrite(pp->fd, &diff, sizeof(ptrdiff_t));
			break;
		default:
			break;
		}

	fullwriteword(pp->fd, pair.file);
	fullwriteword(pp->fd, pair.ctype);
	fullwrite(pp->fd, &pair.ctypepos, sizeof(size_t));
	fullwriteword(pp->fd, pair.xcode);

	/*
	 * We can write a new "val" in the validator allocated on the
	 * heap: if we do, free it here.
	 */

	if (save != pair.val)
		free(pair.val);
}

/*
 * Read full stdin request into memory.
 * This reads at most "len" bytes and NUL-terminates the results, the
 * length of which may be less than "len" and is stored in *szp if not
 * NULL.
 * Returns the pointer to the data.
 * NOTE: we can't use fullread() here because we may not get the total
 * number of bytes requested.
 * NOTE: "szp" can legit be set to zero.
 */
static char *
scanbuf(size_t len, size_t *szp)
{
	ssize_t		 ssz;
	size_t		 sz;
	char		*p;
	int		 rc;
	struct pollfd	 pfd;

	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;

	/* Allocate the entire buffer here. */

	if (NULL == (p = XMALLOC(len + 1)))
		_exit(EXIT_FAILURE);

	/* 
	 * Keep reading til we get all the data or the sender stops
	 * giving us data---whichever comes first.
	 */

	for (sz = 0; sz < len; sz += (size_t)ssz) {
		if ((rc = poll(&pfd, 1, -1)) < 0) {
			XWARN("poll: POLLIN");
			_exit(EXIT_FAILURE);
		} else if (0 == rc) {
			XWARNX("poll: timeout!?");
			ssz = 0;
			continue;
		} else if ( ! (POLLIN & pfd.revents))
			break;

		if ((ssz = read(STDIN_FILENO, p + sz, len - sz)) < 0) {
			XWARNX("read");
			_exit(EXIT_FAILURE);
		} else if (0 == ssz)
			break;
	}

	if (sz < len)
		XWARNX("content size mismatch: have "
			"%zu, wanted %zu\n", sz, len);

	/* ALWAYS NUL-terminate. */

	p[sz] = '\0';
	if (NULL != szp)
		*szp = sz;

	return(p);
}

/*
 * Reset a particular mime component.
 * We can get duplicates, so reallocate.
 */
static void
mime_reset(char **dst, const char *src)
{

	free(*dst);
	if (NULL == (*dst = XSTRDUP(src)))
		_exit(EXIT_FAILURE);
}

/*
 * Free up all MIME headers.
 * We might call this more than once, so make sure that it can be
 * invoked again by setting the memory to zero.
 */
static void
mime_free(struct mime *mime)
{

	free(mime->disp);
	free(mime->name);
	free(mime->file);
	free(mime->ctype);
	free(mime->xcode);
	free(mime->bound);
	memset(mime, 0, sizeof(struct mime));
}

/*
 * Parse out all MIME headers.
 * This is defined by RFC 2045.
 * This returns TRUE if we've parsed up to (and including) the last
 * empty CRLF line, or FALSE if something has gone wrong (e.g., parse
 * error, out of memory).
 * If FALSE, parsing should stop immediately.
 */
static int
mime_parse(const struct parms *pp, struct mime *mime, 
	char *buf, size_t len, size_t *pos)
{
	char	*key, *val, *keyend, *end, *start, *line;
	int	 type;
#define MIMETYPE_UNKNOWN 0
#define	MIMETYPE_TRANSFER_ENCODING 1
#define	MIMETYPE_DISPOSITION 2
#define	MIMETYPE_TYPE 3
	int	 rc = 0;

	mime_free(mime);

	while (*pos < len) {
		/* Each MIME line ends with a CRLF. */

		start = &buf[*pos];
		end = memmem(start, len - *pos, "\r\n", 2);
		if (NULL == end) {
			XWARNX("RFC violation: MIME header "
				"line without CRLF");
			return(0);
		}

		/* 
		 * NUL-terminate to make a nice line.
		 * Then re-set our starting position. 
		 */

		*end = '\0';
		*pos += (end - start) + 2;

		/* Empty CRLF line: we're done here! */

		if ('\0' == *start) {
			rc = 1;
			break;
		}

		/* Find end of MIME statement name. */

		key = start;
		if (NULL == (val = strchr(key, ':'))) {
			XWARNX("RFC violation: MIME header "
				"without key-value separator");
			return(0);
		} else if (key != val) {
			/* 
			 * The RFCs disagree on white-space before the
			 * colon, but as it's allowed in the original
			 * RFC 822 and obsolete syntax should be
			 * supported, we do so here.
			 */
			keyend = val - 1;
			do {
				if (' ' != *keyend) 
					break;
				*keyend = '\0';
				keyend--;
			} while (keyend >= key);
		} else
			XWARNX("RFC warning: empty MIME "
				"header name");

		*val++ = '\0';
		while (' ' == *val)
			val++;

		/* 
		 * Set "line" to be at the MIME value subpart, for
		 * example, "Content-type: text/plain; charset=us-ascii"
		 * would put us at the parts before "charset".
		 */

		line = NULL;
		if (NULL != (line = strchr(val, ';')))
			*line++ = '\0';

		/* 
		 * Allow these specific MIME header statements.
		 * We'll follow up by parsing specific information from
		 * the header values, so remember what we parsed.
		 */

		if (0 == strcasecmp(key, "content-transfer-encoding")) {
			mime_reset(&mime->xcode, val);
			type = MIMETYPE_TRANSFER_ENCODING;
		} else if (0 == strcasecmp(key, "content-disposition")) {
			mime_reset(&mime->disp, val);
			type = MIMETYPE_DISPOSITION;
		} else if (0 == strcasecmp(key, "content-type")) {
			mime_reset(&mime->ctype, val);
			type = MIMETYPE_TYPE;
		} else
			type = MIMETYPE_UNKNOWN;

		/* 
		 * Process subpart only for content-type and
		 * content-disposition.
		 * The rest have no information we want.
		 */

		if (MIMETYPE_TYPE != type &&
		    MIMETYPE_DISPOSITION != type)
			continue;

		while (NULL != (key = line)) {
			while (' ' == *key)
				key++;
			if ('\0' == *key)
				break;

			if (NULL == (val = strchr(key, '='))) {
				XWARNX("RFC violation: MIME header "
					"without subpart separator");
				return(0);
			} else if (key != val) {
				keyend = val - 1;
				/*
				 * It's not clear whether we're allowed
				 * to have OWS before the separator, but
				 * allow for it anyway.
				 */

				do {
					if (' ' != *keyend) 
						break;
					*keyend = '\0';
					keyend--;
				} while (keyend >= key);
			} else 
				XWARNX("RFC warning: empty MIME "
					"subpart name");

			*val++ = '\0';

			/* Quoted string. */

			if ('"' == *val) {
				val++;
				if (NULL == (line = strchr(val, '"'))) {
					XWARNX("RFC violation: MIME header "
						"subpart not terminated");
					return(0);
				}
				*line++ = '\0';

				/* 
				 * It's unclear as to whether this is
				 * allowed (white-space before the
				 * semicolon separator), but let's
				 * accommodate for it anyway.
				 */

				while (' ' == *line)
					line++;
				if (';' == *line)
					line++;
			} else if (NULL != (line = strchr(val, ';')))
				*line++ = '\0';

			/* White-listed sub-commands. */

			switch (type) {
			case (MIMETYPE_DISPOSITION):
				if (0 == strcasecmp(key, "filename"))
					mime_reset(&mime->file, val);
				else if (0 == strcasecmp(key, "name"))
					mime_reset(&mime->name, val);
				break;
			case (MIMETYPE_TYPE):
				if (0 == strcasecmp(key, "boundary"))
					mime_reset(&mime->bound, val);
				break;
			default:
				break;
			}
		}
	} 

	mime->ctypepos = str2ctype(pp, mime->ctype);

	if ( ! rc)
		XWARNX("RFC violation: unexpected EOF "
			"while parsing MIME headers");

	return(rc);
}

/*
 * Parse keys and values separated by newlines.
 * I'm not aware of any standard that defines this, but the W3
 * guidelines for HTML give a rough idea.
 * FIXME: deprecate this.
 */
static void
parse_pairs_text(const struct parms *pp, char *p)
{
	char            *key, *val;

	while (NULL != p && '\0' != *p) {
		/* Skip leading whitespace. */
		while (' ' == *p)
			p++;

		key = p;
		val = NULL;
		if (NULL != (p = strchr(p, '='))) {
			/* Key/value pair. */
			*p++ = '\0';
			val = p;
			p = strstr(val, "\r\n");
			if (NULL != p) {
				*p = '\0';
				p += 2;
			}
		} else {
			/* No value--error. */
			p = strstr(key, "\r\n");
			if (NULL != p) {
				*p = '\0';
				p += 2;
			}
			XWARNX("text key: no value");
			continue;
		}

		if ('\0' == *key) {
			XWARNX("text key: zero-length");
			continue;
		}
		output(pp, key, val, strlen(val), NULL);
	}
}

/*
 * Parse an HTTP message that has a given content-type.
 * This happens with, e.g., PUT requests.
 * We fake up a "name" for this (it's not really a key-value pair) of an
 * empty string, then pass that to the validator and forwarder.
 */
static void
parse_body(const char *ct, const struct parms *pp, char *b, size_t bsz)
{
	char		 name;
	struct mime	 mime;

	memset(&mime, 0, sizeof(struct mime));

	if (NULL == (mime.ctype = XSTRDUP(ct)))
		_exit(EXIT_FAILURE);
	mime.ctypepos = str2ctype(pp, mime.ctype);

	name = '\0';
	output(pp, &name, b, bsz, &mime);
	free(mime.ctype);
}

/*
 * In-place HTTP-decode a string.  The standard explanation is that this
 * turns "%4e+foo" into "n foo" in the regular way.  This is done
 * in-place over the allocated string.
 * Returns zero on decoding failure, non-zero otherwise.
 */
static int
urldecode(char *p)
{
	char             hex[3];
	unsigned int	 c;

	hex[2] = '\0';

	for ( ; '\0' != *p; p++) {
		if ('%' == *p) {
			if ('\0' == (hex[0] = *(p + 1))) {
				XWARNX("urldecode: short hex");
				return(0);
			} else if ('\0' == (hex[1] = *(p + 2))) {
				XWARNX("urldecode: short hex");
				return(0);
			} else if (1 != sscanf(hex, "%x", &c)) {
				XWARNX("urldecode: bad hex");
				return(0);
			} else if ('\0' == c) {
				XWARNX("urldecode: NUL byte");
				return(0);
			}

			*p = c;
			memmove(p + 1, p + 3, strlen(p + 3) + 1);
		} else if ('+' == *p)
			*p = ' ';
	}

	return(1);
}

/*
 * Parse out key-value pairs from an HTTP cookie.
 * These are not URL encoded (at this phase): they're just simple
 * key-values "crumbs" with opaque values.
 * This is defined by RFC 6265, however, we don't [yet] do the
 * quoted-string implementation, nor do we check for accepted
 * characters so long as the delimiters aren't used.
 */
static void
parse_pairs(const struct parms *pp, char *p)
{
	char            *key, *val;

	while (NULL != p && '\0' != *p) {
		/* Skip leading whitespace. */
		while (' ' == *p)
			p++;

		key = p;
		val = NULL;
		if (NULL != (p = strchr(p, '='))) {
			/* Key/value pair. */
			*p++ = '\0';
			val = p;
			p = strchr(p, ';');
			if (NULL != p)
				*p++ = '\0';
		} else {
			/* No value--error. */
			p = strchr(key, ';');
			if (NULL != p)
				p++;
			XWARNX("cookie key: no value");
			continue;
		}

		if ('\0' == *key) {
			/* This is sort-of allowed. */
			XWARNX("cookie key: zero length");
			continue;
		}

		output(pp, key, val, strlen(val), NULL);
	}
}

/*
 * Parse out key-value pairs from an HTTP request variable.
 * This is either a POST or GET string.
 * This MUST be a non-binary (i.e., NUL-terminated) string!
 */
static void
parse_pairs_urlenc(const struct parms *pp, char *p)
{
	char	*key, *val;
	char	 empty = '\0';
	size_t	 sz;

	assert(NULL != p);

	while ('\0' != *p) {
		/* Skip leading whitespace. */
		while (' ' == *p)
			p++;

		key = p;
		val = NULL;

		if (NULL != (p = strchr(p, '='))) {
			/* Key/value pair. */
			*p++ = '\0';
			val = p;
			sz = strcspn(p, ";&");
			p += sz;
			if ('\0' != *p)
				*p++ = '\0';
		} else {
			/* 
			 * No value.
			 * We let this through and just specify that it
			 * has an empty value.
			 * There is no standard that says what we do,
			 * but the information should pass through.
			 */
			p = key;
			sz = strcspn(p, ";&");
			p += sz;
			if ('\0' != *p)
				p++;
			val = &empty;
		}

		/*
		 * Both the key and the value can be URL encoded, so
		 * decode those into the character string now.
		 * If decoding fails, don't decode the given pair, but
		 * instead move on to the next one after logging the
		 * failure.
		 */

		if ('\0' == *key)
			XWARNX("url key: zero length");
		else if ( ! urldecode(key))
			XWARNX("url key: key decode");
		else if (NULL != val && ! urldecode(val))
			XWARNX("url key: val decode");
		else
			output(pp, key, val, strlen(val), NULL);
	}
}

/*
 * This is described by the "multipart-body" BNF part of RFC 2046,
 * section 5.1.1.
 * We return TRUE if the parse was ok, FALSE if errors occurred (all
 * calling parsers should bail too).
 */
static int
parse_multiform(const struct parms *pp, char *name, 
	const char *bound, char *buf, size_t len, size_t *pos)
{
	struct mime	 mime;
	size_t		 endpos, bbsz, partsz;
	char		*ln, *bb;
	int		 rc, first;

	/* Define our buffer boundary. */
	
	if ((rc = XASPRINTF(&bb, "\r\n--%s", bound)) < 0)
		_exit(EXIT_FAILURE);

	assert(rc > 0);
	bbsz = rc;
	rc = 0;

	memset(&mime, 0, sizeof(struct mime));

	/* Read to the next instance of a buffer boundary. */

	for (first = 1; *pos < len; first = 0, *pos = endpos) {
		/*
		 * The (first ? 2 : 0) is because the first prologue
		 * boundary will not incur an initial CRLF, so our bb is
		 * past the CRLF and two bytes smaller.
		 */

		ln = memmem(&buf[*pos], len - *pos, 
			bb + (first ? 2 : 0), 
			bbsz - (first ? 2 : 0));

		if (NULL == ln) {
			XWARNX("RFC violation: unexpected "
				"EOF when scanning for boundary");
			goto out;
		}

		/* 
		 * Set "endpos" to point to the beginning of the next
		 * multipart component, i.e, the end of the boundary
		 * "bb" string.
		 * Again, be respectful of whether we should scan after
		 * the lack of initial CRLF.
		 */

		endpos = *pos + (ln - &buf[*pos]) + 
			bbsz - (first ? 2 : 0);

		/* Check buffer space. */

		if (endpos > len - 2) {
			XWARNX("RFC violation: multipart section "
				"writes into trailing CRLF");
			goto out;
		}

		/* 
		 * Terminating boundary has an initial trailing "--".
		 * If not terminating, must be followed by a CRLF.
		 * If terminating, RFC 1341 says we can ignore whatever
		 * comes after the last boundary.
		 */

		if (memcmp(&buf[endpos], "--", 2)) {
			while (endpos < len && ' ' == buf[endpos])
				endpos++;
			if (endpos > len - 2 ||
			    memcmp(&buf[endpos], "\r\n", 2)) {
				XWARNX("RFC violation: multipart "
					"boundary without CRLF");
				goto out;
			}
			endpos += 2;
		} else
			endpos = len;

		/* First section: jump directly to reprocess. */

		if (first)
			continue;

		/* 
		 * Zero-length part.
		 * This shouldn't occur, but if it does, it'll screw up
		 * the MIME parsing (which requires a blank CRLF before
		 * considering itself finished).
		 */

		if (0 == (partsz = ln - &buf[*pos])) {
			XWARNX("RFC violation: zero-length "
				"multipart section");
			continue;
		}

		/* We now read our MIME headers, bailing on error. */

		if ( ! mime_parse(pp, &mime, buf, *pos + partsz, pos)) {
			XWARNX("nested error: MIME headers");
			goto out;
		}

		/* 
		 * As per RFC 2388, we need a name and disposition. 
		 * Note that multipart/mixed bodies will inherit the
		 * name of their parent, so the mime.name is ignored.
		 */

		if (NULL == mime.name && NULL == name) {
			XWARNX("RFC violation: no MIME name");
			continue;
		} else if (NULL == mime.disp) {
			XWARNX("RFC violation: no MIME disposition");
			continue;
		}

		/* As per RFC 2045, we default to text/plain. */

		if (NULL == mime.ctype &&
		    NULL == (mime.ctype = XSTRDUP("text/plain")))
			_exit(EXIT_FAILURE);

		partsz = ln - &buf[*pos];

		/* 
		 * Multipart sub-handler. 
		 * We only recognise the multipart/mixed handler.
		 * This will route into our own function, inheriting the
		 * current name for content.
		 */

		if (0 == strcasecmp(mime.ctype, "multipart/mixed")) {
			if (NULL == mime.bound) {
				XWARNX("RFC violation: missing "
					"mixed multipart boundary");
				goto out;
			}
			if ( ! parse_multiform
				(pp, NULL != name ? name :
				 mime.name, mime.bound, buf, 
				 *pos + partsz, pos)) {
				XWARNX("nested error: mixed "
					"multipart section parse");
				goto out;
			}
			continue;
		}

		assert('\r' == buf[*pos + partsz] || 
		       '\0' == buf[*pos + partsz]);

		if ('\0' != buf[*pos + partsz])
			buf[*pos + partsz] = '\0';

		/* Assign all of our key-value pair data. */

		output(pp, NULL != name ? name : 
			mime.name, &buf[*pos], partsz, &mime);
	}

	/*
	 * According to the specification, we can have transport
	 * padding, a CRLF, then the epilogue.
	 * But since we don't care about that crap, just pretend that
	 * everything's fine and exit.
	 */

	rc = 1;
out:
	free(bb);
	mime_free(&mime);
	return(rc);
}

/*
 * Parse the boundary from a multipart CONTENT_TYPE and pass it to the
 * actual parsing engine.
 * This doesn't actually handle any part of the MIME specification.
 */
static void
parse_multi(const struct parms *pp, char *line, char *b, size_t bsz)
{
	char		*cp;
	size_t		 len = 0;

	while (' ' == *line)
		line++;
	if (';' != *line++) {
		XWARNX("RFC violation: expected semicolon following "
			"multipart/form-data declaration");
		return;
	}
	while (' ' == *line)
		line++;

	/* We absolutely need the boundary marker. */
	if (strncmp(line, "boundary", 8)) {
		XWARNX("RFC violation: expected \"boundary\" "
			"following multipart/form-data declaration");
		return;
	}
	line += 8;
	while (' ' == *line)
		line++;

	if ('=' != *line++) {
		XWARNX("RFC violation: expected key-value "
			"for multipart/form-data boundary");
		return;
	}

	while (' ' == *line)
		line++;

	/* Make sure the line is terminated in the right place. */

	if ('"' == *line) {
		if (NULL == (cp = strchr(++line, '"'))) {
			XWARNX("RFC violation: unterminated "
				"boundary quoted string");
			return;
		}
		*cp = '\0';
	} else {
		/*
		 * XXX: this may not properly follow RFC 2046, 5.1.1,
		 * which specifically lays out the boundary characters.
		 * We simply jump to the first whitespace.
		 */
		line[strcspn(line, " ")] = '\0';
	}

	/*
	 * If we have data following the boundary declaration, we simply
	 * ignore it.
	 * The RFC mandates the existence of the boundary, but is silent
	 * as to whether anything can come after it.
	 */

	parse_multiform(pp, NULL, line, b, bsz, &len);
}

/*
 * Output all of the HTTP_xxx headers.
 * This transforms the HTTP_xxx header (CGI form) into HTTP form, which
 * is the second part title-cased, e.g., HTTP_FOO = Foo.
 * Disallow zero-length values as per RFC 3875, 4.1.18.
 */
static void
kworker_child_env(const struct env *env, int fd, size_t envsz)
{
	size_t	 	 i, j, sz, reqs;
	int		 first;
	enum krequ	 requ;
	char		 c;
	const char	*cp;

	for (reqs = i = 0; i < envsz; i++)
		if (0 == strncmp(env[i].key, "HTTP_", 5) &&
		    '\0' != env[i].key[5])
			reqs++;

	fullwrite(fd, &reqs, sizeof(size_t));

	for (i = 0; i < envsz; i++) {
		/*
		 * First, search for the key name (HTTP_XXX) in our list
		 * of known headers.
		 * We must have non-zero-length keys.
		 */

		if (strncmp(env[i].key, "HTTP_", 5) || 
		    '\0' == env[i].key[5])
			continue;

		for (requ = 0; requ < KREQU__MAX; requ++)
			if (0 == strcmp(krequs[requ], env[i].key))
				break;

		fullwrite(fd, &requ, sizeof(enum krequ));

		/*
		 * According to RFC 3875, 4.1.18, HTTP headers are
		 * re-written into CGI environment variables by
		 * uppercasing and converting dashes to underscores.
		 * In this part, we [try to] reverse that so that the
		 * headers are properly identified.
		 * (We also skip the HTTP_ leading part.)
		 */

		sz = env[i].keysz - 5;
		cp = env[i].key + 5;
		fullwrite(fd, &sz, sizeof(size_t));
		for (j = 0, first = 1; j < sz; j++) {
			if ('_' == cp[j]) {
				c = '-';
				first = 1;
			} else if (first) {
				c = cp[j];
				first = 0;
			} else
				c = tolower((unsigned char)cp[j]);
			fullwrite(fd, &c, 1);
		}

		fullwrite(fd, &env[i].valsz, sizeof(size_t));
		fullwrite(fd, env[i].val, env[i].valsz);
	}
}

/*
 * Like getenv() but for our env structure.
 */
static char *
kworker_env(struct env *env, size_t envsz, const char *key)
{
	size_t	 i;

	for (i = 0; i < envsz; i++) 
		if (0 == strcmp(env[i].key, key))
			return(env[i].val);
	return(NULL);
}

/*
 * Output the method found in our environment.
 * Returns the method.
 * Defaults to KMETHOD_GET, uses KETHOD__MAX if the method was bad.
 */
static enum kmethod
kworker_child_method(struct env *env, int fd, size_t envsz)
{
	enum kmethod	 meth;
	const char	*cp;

	/* RFC 3875, 4.1.12. */
	/* We assume GET if not supplied. */

	meth = KMETHOD_GET;
	if (NULL != (cp = kworker_env(env, envsz, "REQUEST_METHOD")))
		for (meth = 0; meth < KMETHOD__MAX; meth++)
			if (0 == strcmp(kmethods[meth], cp))
				break;
	fullwrite(fd, &meth, sizeof(enum kmethod));
	return(meth);
}

/*
 * Output the web server's authentication.
 * Defaults to KAUTH_NONE.
 */
static void
kworker_child_auth(struct env *env, int fd, size_t envsz)
{
	enum kauth	 auth;
	const char	*cp;	

	/* Determine authentication: RFC 3875, 4.1.1. */

	auth = KAUTH_NONE;
	if (NULL != (cp = kworker_env(env, envsz, "AUTH_TYPE")))
		for (auth = 0; auth < KAUTH_UNKNOWN; auth++) {
			if (NULL == kauths[auth])
				continue;
			if (0 == strcmp(kauths[auth], cp))
				break;
		}
	fullwrite(fd, &auth, sizeof(enum kauth));
}

/*
 * Send the raw (i.e., un-webserver-filtered) authorisation to the
 * parent.
 * Most web servers will `handle this for us'.  Ugh.
 */
static int
kworker_child_rawauth(struct env *env, int fd, size_t envsz)
{

	return(kworker_auth_child(fd, kworker_env
		(env, envsz, "HTTP_AUTHORIZATION")));
}

/*
 * Send our HTTP scheme (secure or not) to the parent.
 */
static void
kworker_child_scheme(struct env *env, int fd, size_t envsz)
{
	const char	*cp;
	enum kscheme	 scheme;

	/* 
	 * This isn't defined in any RFC.
	 * It seems to be the best way of getting whether we're HTTPS,
	 * as the SERVER_PROTOCOL (RFC 3875, 4.1.16) doesn't reliably
	 * return the scheme.
	 */
	if (NULL == (cp = kworker_env(env, envsz, "HTTPS")))
		cp = "off";
	scheme = 0 == strcasecmp(cp, "on") ?
		KSCHEME_HTTPS : KSCHEME_HTTP;
	fullwrite(fd, &scheme, sizeof(enum kscheme));
}

/*
 * Send remote address to the parent.
 * This is required by RFC 3875, 4.1.8.
 * Use 127.0.0.1 on protocol violation.
 */
static void
kworker_child_remote(struct env *env, int fd, size_t envsz)
{
	const char	*cp;

	if (NULL == (cp = kworker_env(env, envsz, "REMOTE_ADDR"))) {
		XWARNX("RFC violation: REMOTE_ADDR not set");
		cp = "127.0.0.1";
	}

	fullwriteword(fd, cp);
}

/*
 * Parse and send the port to the parent.
 * This is required by RFC 3875, 4.1.15.
 * Use port 80 if not provided or on parse error.
 */
static void
kworker_child_port(struct env *env, int fd, size_t envsz)
{
	uint16_t	 port;
	const char	*cp, *er;

	port = 80;
	if (NULL != (cp = kworker_env(env, envsz, "SERVER_PORT"))) {
		port = strtonum(cp, 0, UINT16_MAX, &er);
		if (NULL != er) {
			XWARNX("RFC violation: invalid SERVER_PORT");
			port = 80;
		}
	} else
		XWARNX("RFC violation: SERVER_PORT not set");

	fullwrite(fd, &port, sizeof(uint16_t));
}

/*
 * Send requested host to the parent.
 * This is required by RFC 7230, 5.4.
 * Use "localhost" if not provided.
 */
static void
kworker_child_httphost(struct env *env, int fd, size_t envsz)
{
	const char	*cp;

	if (NULL == (cp = kworker_env(env, envsz, "HTTP_HOST"))) {
		XWARNX("RFC violation: HTTP_HOST not set");
		cp = "localhost";
	}

	fullwriteword(fd, cp);
}

/* 
 * Send script name to the parent.
 * This is required by RFC 3875, 4.1.13.
 * Use the empty string on error.
 */
static void
kworker_child_scriptname(struct env *env, int fd, size_t envsz)
{
	const char	*cp;

	if (NULL == (cp = kworker_env(env, envsz, "SCRIPT_NAME"))) {
		XWARNX("RFC violation: SCRIPT_NAME not set");
		cp = "";
	}

	fullwriteword(fd, cp);
}

/*
 * Parse all path information (subpath, path, etc.) and send to parent.
 */
static void
kworker_child_path(struct env *env, int fd, size_t envsz)
{
	char	*cp, *ep, *sub;
	size_t	 len;

	/*
	 * Parse the first path element (the page we want to access),
	 * subsequent path information, and the file suffix.  We convert
	 * suffix and path element into the respective enum's inline.
	 */
	cp = kworker_env(env, envsz, "PATH_INFO");
	fullwriteword(fd, cp);

	/* This isn't possible in the real world. */
	if (NULL != cp && '/' == *cp)
		cp++;

	if (NULL != cp && '\0' != *cp) {
		ep = cp + strlen(cp) - 1;
		while (ep > cp && '/' != *ep && '.' != *ep)
			ep--;

		/* Start with writing our suffix. */
		if ('.' == *ep) {
			*ep++ = '\0';
			fullwriteword(fd, ep);
		} else
			fullwriteword(fd, NULL);

		/* Now find the top-most path part. */
		if (NULL != (sub = strchr(cp, '/')))
			*sub++ = '\0';

		/* Send the base path. */
		fullwriteword(fd, cp);

		/* Send the path part. */
		fullwriteword(fd, sub);
	} else {
		len = 0;
		/* Suffix, base path, and path part. */
		fullwrite(fd, &len, sizeof(size_t));
		fullwrite(fd, &len, sizeof(size_t));
		fullwrite(fd, &len, sizeof(size_t));
	}
}

/*
 * Construct the "HA2" component of an HTTP digest hash.
 * See RFC 2617.
 * We only do this if our authorisation requires it!
 */
static void
kworker_child_bodymd5(struct env *env, int fd, 
	size_t envsz, const char *b, size_t bsz, int md5)
{
	MD5_CTX		 ctx;
	unsigned char 	 ha2[MD5_DIGEST_LENGTH];
	const char 	*uri, *script, *method;
	size_t		 sz;

	if ( ! md5) {
		sz = 0;
		fullwrite(fd, &sz, sizeof(size_t));
		return;
	}

	uri = kworker_env(env, envsz, "PATH_INFO");
	script = kworker_env(env, envsz, "SCRIPT_NAME");
	method = kworker_env(env, envsz, "REQUEST_METHOD");

	if (NULL == uri)
		uri = "";
	if (NULL == script)
		script = "";
	if (NULL == method)
		method = "";

	MD5Init(&ctx);
	MD5Updatec(&ctx, method, strlen(method));
	MD5Updatec(&ctx, ":", 1);
	MD5Updatec(&ctx, script, strlen(script));
	MD5Updatec(&ctx, uri, strlen(uri));
	MD5Updatec(&ctx, ":", 1);
	MD5Updatec(&ctx, b, bsz);
	MD5Final(ha2, &ctx);

	/* This is a binary write! */
	sz = MD5_DIGEST_LENGTH;
	fullwrite(fd, &sz, sizeof(size_t));
	fullwrite(fd, ha2, sz);
}

/*
 * Parse and send the body of the request to the parent.
 * This is arguably the most complex part of the system.
 */
static void
kworker_child_body(struct env *env, int fd, size_t envsz,
	struct parms *pp, enum kmethod meth, char *b, 
	size_t bsz, unsigned int debugging, int md5)
{
	size_t 	 i, len, cur;
	char	*cp, *bp = b;

	/*
	 * The CONTENT_LENGTH must be a valid integer.
	 * Since we're storing into "len", make sure it's in size_t.
	 * If there's an error, it will default to zero.
	 * Note that LLONG_MAX < SIZE_MAX.
	 * RFC 3875, 4.1.2.
	 */

	len = 0;
	if (NULL != (cp = kworker_env(env, envsz, "CONTENT_LENGTH")))
		len = strtonum(cp, 0, LLONG_MAX, NULL);

	if (0 == len) {
		/* Remember to print our MD5 value. */
		kworker_child_bodymd5(env, fd, envsz, "", 0, md5);
		return;
	}

	/* Check FastCGI input lengths. */

	if (NULL != bp && bsz != len)
		XWARNX("real and reported content lengths differ");

	/*
	 * If a CONTENT_TYPE has been specified (i.e., POST or GET has
	 * been set -- we don't care which), then switch on that type
	 * for parsing out key value pairs.
	 * RFC 3875, 4.1.3.
	 * HTML5, 4.10.
	 * We only support the main three content types.
	 */

	pp->type = IN_FORM;
	cp = kworker_env(env, envsz, "CONTENT_TYPE");

	/* 
	 * If we're CGI, read the request now.
	 * Note that the "bsz" can come out as zero.
	 */

	if (NULL == b)
		b = scanbuf(len, &bsz);

	assert(NULL != b);

	/* If requested, print our MD5 value. */

	kworker_child_bodymd5(env, fd, envsz, b, bsz, md5);

	if (bsz && KREQ_DEBUG_READ_BODY & debugging) {
		fprintf(stderr, "%u: ", getpid());
		for (cur = i = 0; i < bsz; i++, cur++) {
			/* Print at most BUFSIZ characters. */
			if (BUFSIZ == cur) {
				fputc('\n', stderr);
				fflush(stderr);
				fprintf(stderr, "%u: ", getpid());
				cur = 0;
			}

			/* Filter output. */
			if (isprint((unsigned char)b[i]) || '\n' == b[i])
				fputc(b[i], stderr);
			else if ('\t' == b[i])
				fputs("\\t", stderr);
			else if ('\r' == b[i])
				fputs("\\r", stderr);
			else if ('\v' == b[i])
				fputs("\\v", stderr);
			else if ('\b' == b[i])
				fputs("\\b", stderr);
			else
				fputc('?', stderr);

			/* Handle newline. */
			if ('\n' == b[i]) {
				cur = 0;
				fflush(stderr);
				fprintf(stderr, "%u: ", getpid());
			}
		}
		/* Terminate with newline. */
		if ('\n' != b[bsz - 1])
			fputc('\n', stderr);
		/* Print some statistics. */
		fprintf(stderr, "%u: %zu B rx\n", getpid(), bsz);
		fflush(stderr);
	}

	if (NULL != cp) {
		if (0 == strcasecmp(cp, "application/x-www-form-urlencoded"))
			parse_pairs_urlenc(pp, b);
		else if (0 == strncasecmp(cp, "multipart/form-data", 19)) 
			parse_multi(pp, cp + 19, b, bsz);
		else if (KMETHOD_POST == meth && 0 == strcasecmp(cp, "text/plain"))
			parse_pairs_text(pp, b);
		else
			parse_body(cp, pp, b, bsz);
	} else
		parse_body(kmimetypes[KMIME_APP_OCTET_STREAM], pp, b, bsz);

	/* Free CGI parsed buffer (FastCGI is done elsewhere). */

	if (NULL == bp)
		free(b);
}

/*
 * Send query string data to parent.
 * Even POST requests are allowed to have QUERY_STRING elements.
 * Note: both QUERY_STRING and CONTENT_TYPE fields share the same field
 * space.
 */
static void
kworker_child_query(struct env *env, 
	int fd, size_t envsz, struct parms *pp)
{
	char 	*cp;

	pp->type = IN_QUERY;
	if (NULL != (cp = kworker_env(env, envsz, "QUERY_STRING")))
		parse_pairs_urlenc(pp, cp);
}

/*
 * Send cookies to our parent.
 * These use the same syntax as QUERY_STRING elements, but don't share
 * the same namespace (just as a means to differentiate the same names).
 */
static void
kworker_child_cookies(struct env *env, 
	int fd, size_t envsz, struct parms *pp)
{
	char	*cp;

	pp->type = IN_COOKIE;
	if (NULL != (cp = kworker_env(env, envsz, "HTTP_COOKIE")))
		parse_pairs(pp, cp);
}

/*
 * Terminate the input fields for the parent. 
 */
static void
kworker_child_last(int fd)
{
	enum input last = IN__MAX;

	fullwrite(fd, &last, sizeof(enum input));
}

/*
 * This is the child kcgi process that's going to do the unsafe reading
 * of network data to parse input.
 * When it parses a field, it outputs the key, key size, value, and
 * value size along with the field type.
 * We use the CGI specification in RFC 3875.
 */
enum kcgi_err
kworker_child(int wfd,
	const struct kvalid *keys, size_t keysz, 
	const char *const *mimes, size_t mimesz,
	unsigned int debugging)
{
	struct parms	  pp;
	char		 *cp;
	const char	 *start;
	char		**evp;
	int		  md5;
	enum kmethod	  meth;
	size_t	 	  i;
	extern char	**environ;
	struct env	 *envs = NULL;
	size_t		  envsz;

	pp.fd = wfd;
	pp.keys = keys;
	pp.keysz = keysz;
	pp.mimes = mimes;
	pp.mimesz = mimesz;

	/*
	 * Pull the entire environment into an array.
	 */
	for (envsz = 0, evp = environ; NULL != *evp; evp++) 
		envsz++;

	if (envsz) {
		envs = XCALLOC(envsz, sizeof(struct env));
		if (NULL == envs)
			return(KCGI_ENOMEM);
	}

	/* 
	 * Pull all reasonable values from the environment into "envs".
	 * Filter out variables that don't meet RFC 3875, section 4.1.
	 * However, we're a bit more relaxed: we don't let through
	 * zero-length, non-ASCII, control characters, and whitespace.
	 */

	for (i = 0, evp = environ; NULL != *evp; evp++) {
		if (NULL == (cp = strchr(*evp, '=')) ||
		    cp == *evp)
			continue;
		for (start = *evp; '=' != *start; start++)
			if ( ! isascii((unsigned char)*start) ||
			     ! isgraph((unsigned char)*start))
				break;

		/* 
		 * This means something is seriously wrong, so make sure
		 * that the operator knows.
		 */

		if ('=' != *start) {
			XWARNX("RFC violation: bad character "
				"in environment array");
			continue;
		}

		assert(i < envsz);

		if (NULL == (envs[i].key = XSTRDUP(*evp)))
			_exit(EXIT_FAILURE);
		envs[i].val = strchr(envs[i].key, '=');
		*envs[i].val++ = '\0';
		envs[i].keysz = strlen(envs[i].key);
		envs[i].valsz = strlen(envs[i].val);
		i++;
	}

	/* Reset this, accounting for crappy entries. */

	envsz = i;

	/*
	 * Now run a series of transmissions based upon what's in our
	 * environment.
	 */

	kworker_child_env(envs, wfd, envsz);
	meth = kworker_child_method(envs, wfd, envsz);
	kworker_child_auth(envs, wfd, envsz);
	md5 = kworker_child_rawauth(envs, wfd, envsz);
	kworker_child_scheme(envs, wfd, envsz);
	kworker_child_remote(envs, wfd, envsz);
	kworker_child_path(envs, wfd, envsz);
	kworker_child_scriptname(envs, wfd, envsz);
	kworker_child_httphost(envs, wfd, envsz);
	kworker_child_port(envs, wfd, envsz);

	/* And now the message body itself. */

	kworker_child_body(envs, wfd, envsz, 
		&pp, meth, NULL, 0, debugging, md5);
	kworker_child_query(envs, wfd, envsz, &pp);
	kworker_child_cookies(envs, wfd, envsz, &pp);
	kworker_child_last(wfd);

	/* Note: the "val" is from within the key. */

	for (i = 0; i < envsz; i++) 
		free(envs[i].key);
	free(envs);
	return(KCGI_OK);
}

/*
 * Read the FastCGI header (see section 8, Types and Contents,
 * FCGI_Header, in the FastCGI Specification v1.0).
 * Return zero if have data, non-zero on success.
 */
static struct fcgi_hdr *
kworker_fcgi_header(int fd, struct fcgi_hdr *hdr, unsigned char *buf)
{
	enum kcgi_err	 er;
	struct fcgi_hdr	*ptr;
	int		 rc;

	if ((rc = fullread(fd, buf, 8, 0, &er)) < 0) {
		XWARNX("failed read FastCGI header");
		return(NULL);
	} 

	/* Translate from network-byte order. */

	ptr = (struct fcgi_hdr *)buf;
	hdr->version = ptr->version;
	hdr->type = ptr->type;
	hdr->requestId = ntohs(ptr->requestId);
	hdr->contentLength = ntohs(ptr->contentLength);
	hdr->paddingLength = ptr->paddingLength;
#if 0
	fprintf(stderr, "%s: DEBUG version: %" PRIu8 "\n", 
		__func__, hdr->version);
	fprintf(stderr, "%s: DEBUG type: %" PRIu8 "\n", 
		__func__, hdr->type);
	fprintf(stderr, "%s: DEBUG requestId: %" PRIu16 "\n", 
		__func__, hdr->requestId);
	fprintf(stderr, "%s: DEBUG contentLength: %" PRIu16 "\n", 
		__func__, hdr->contentLength);
	fprintf(stderr, "%s: DEBUG paddingLength: %" PRIu8 "\n", 
		__func__, hdr->paddingLength);
#endif
	if (1 != hdr->version) {
		XWARNX("bad FastCGI header version");
		return(NULL);
	}
	return(hdr);
}

/*
 * Read the content from an FastCGI request.
 * We might need to expand the buffer holding the content.
 */
static int
kworker_fcgi_content(int fd, const struct fcgi_hdr *hdr,
	unsigned char **buf, size_t *bufmaxsz)
{
	void		*ptr;
	enum kcgi_err	 er;

	if (hdr->contentLength > *bufmaxsz) {
		*bufmaxsz = hdr->contentLength;
		if (NULL == (ptr = XREALLOC(*buf, *bufmaxsz)))
			return(0);
		*buf = ptr;
	}
	return(fullread(fd, *buf, hdr->contentLength, 0, &er));
}

/*
 * Read in the entire header and data for the begin sequence request.
 * This is defined in section 5.1 of the v1.0 specification.
 */
static struct fcgi_bgn *
kworker_fcgi_begin(int fd, struct fcgi_bgn *bgn,
	unsigned char **b, size_t *bsz, uint16_t *rid)
{
	struct fcgi_hdr	*hdr, realhdr;
	struct fcgi_bgn	*ptr;
	enum kcgi_err	 er;

	/* 
	 * Read the header entry.
	 * Our buffer is initialised to handle this. 
	 */
	assert(*bsz >= 8);
	if (NULL == (hdr = kworker_fcgi_header(fd, &realhdr, *b)))
		return(NULL);
	*rid = hdr->requestId;

	/* Read the content and discard padding. */
	if (FCGI_BEGIN_REQUEST != hdr->type) {
		XWARNX("unexpected FastCGI header type");
		return(NULL);
	} else if ( ! kworker_fcgi_content(fd, hdr, b, bsz)) {
		XWARNX("failed read FastCGI begin content");
		return(NULL);
	} else if (0 == fulldiscard(fd, hdr->paddingLength, &er)) {
		XWARNX("failed discard FastCGI begin padding");
		return(NULL);
	}

	/* Translate network-byte order. */
	ptr = (struct fcgi_bgn *)*b;
	bgn->role = ntohs(ptr->role);
	bgn->flags = ptr->flags;
	if (0 != bgn->flags) {
		XWARNX("FCGI_KEEP_CONN is not supported");
		return(NULL);
	}
#if 0
	fprintf(stderr, "%s: DEBUG role: %" PRIu16 "\n", 
		__func__, bgn->role);
	fprintf(stderr, "%s: DEBUG flags: %" PRIu8 "\n", 
		__func__, bgn->flags);
#endif
	return(bgn);
}

/*
 * Read in a data stream as defined within section 5.3 of the v1.0
 * specification.
 * We might have multiple stdin buffers for the same data, so always
 * append to the existing.
 */
static int
kworker_fcgi_stdin(int fd, const struct fcgi_hdr *hdr,
	unsigned char **bp, size_t *bsz, 
	unsigned char **sbp, size_t *ssz)
{
	enum kcgi_err	 er;
	void		*ptr;

	/* 
	 * FIXME: there's no need to read this into yet another buffer:
	 * just copy it directly into our accumulating buffer `sbp'.
	 */
	/* Read the content and discard the padding. */
	if (hdr->contentLength > 0 && ! kworker_fcgi_content(fd, hdr, bp, bsz)) {
		XWARNX("failed read FastCGI stdin content");
		return(-1);
	} else if (0 == fulldiscard(fd, hdr->paddingLength, &er)) {
		XWARNX("failed discard FastCGI stdin padding");
		return(-1);
	} 
	
	/* Always NUL-terminate! */
	ptr = XREALLOC(*sbp, *ssz + hdr->contentLength + 1);
	if (NULL == ptr)
		return(-1);
	*sbp = ptr;
	memcpy(*sbp + *ssz, *bp, hdr->contentLength);
	(*sbp)[*ssz + hdr->contentLength] = '\0';
	*ssz += hdr->contentLength;
#if 0
	fprintf(stderr, "%s: DEBUG data: %" PRIu16 " bytes\n", 
		__func__, hdr->contentLength);
#endif
	return(hdr->contentLength > 0);
}

/*
 * Read out a series of parameters contained within a FastCGI parameter
 * request defined in section 5.2 of the v1.0 specification.
 */
static int
kworker_fcgi_params(int fd, const struct fcgi_hdr *hdr,
	unsigned char **bp, size_t *bsz,
	struct env **envs, size_t *envsz)
{
	size_t	 	 i, remain, pos, keysz, valsz;
	unsigned char	*b;
	enum kcgi_err	 er;
	void		*ptr;

	/* Read the content and discard the padding. */

	if ( ! kworker_fcgi_content(fd, hdr, bp, bsz)) {
		XWARNX("failed read FastCGI param content");
		return(0);
	} else if (0 == fulldiscard(fd, hdr->paddingLength, &er)) {
		XWARNX("failed discard FastCGI param padding");
		return(0);
	}

	/*
	 * Loop through the string data that's laid out as a key length
	 * then value length, then key, then value.
	 * There can be arbitrarily many key-values per string.
	 */

	b = *bp;
	remain = hdr->contentLength;
	pos = 0;
	while (remain > 0) {
		/* First read the lengths. */
		assert(pos < hdr->contentLength);
		if (0 != b[pos] >> 7) {
			if (remain <= 3) {
				XWARNX("invalid FastCGI params data");
				return(0);
			}
			keysz = ((b[pos] & 0x7f) << 24) + 
				  (b[pos + 1] << 16) + 
				  (b[pos + 2] << 8) + b[pos + 3];
			pos += 4;
			remain -= 4;
		} else {
			keysz = b[pos];
			pos++;
			remain--;
		}
		if (remain < 1) {
			XWARNX("invalid FastCGI params data");
			return(0);
		}
		assert(pos < hdr->contentLength);
		if (0 != b[pos] >> 7) {
			if (remain <= 3) {
				XWARNX("invalid FastCGI params data");
				return(0);
			}
			valsz = ((b[pos] & 0x7f) << 24) + 
				  (b[pos + 1] << 16) + 
				  (b[pos + 2] << 8) + b[pos + 3];
			pos += 4;
			remain -= 4;
		} else {
			valsz = b[pos];
			pos++;
			remain--;
		}

		/* Make sure we have room for data. */

		if (pos + keysz + valsz > hdr->contentLength) {
			XWARNX("invalid FastCGI params data");
			return(0);
		}

		remain -= keysz;
		remain -= valsz;

		/* 
		 * First, make sure that the key is valid.
		 * There's no documented precedent for this, so we
		 * follow CGI's constraints in RFC 3875, sec. 4.1.
		 * If it's not valid, just skip it.
		 */

		for (i = 0; i < keysz; i++)
			if ( ! isascii((unsigned char)b[pos + i]) ||
			     ! isgraph((unsigned char)b[pos + i]))
				break;

		if (0 == keysz || i < keysz) {
			XWARNX("RFC violation: bad character "
				"in environment parameters");
			pos += keysz + valsz;
			continue;
		}


		/* Look up the key in our existing keys. */

		for (i = 0; i < *envsz; i++) {
			if ((*envs)[i].keysz != keysz)
				continue;
			if (0 == memcmp((*envs)[i].key, &b[pos], keysz))
				break;
		}

		/* 
		 * If we don't have the key: expand our table. 
		 * If we do, clear the current value.
		 */

		if (i == *envsz) {
			ptr = XREALLOCARRAY
				(*envs, *envsz + 1, 
				 sizeof(struct env));
			if (NULL == ptr)
				return(0);

			*envs = ptr;
			(*envs)[i].key = XMALLOC(keysz + 1);
			if (NULL == (*envs)[i].key)
				return(0);

			memcpy((*envs)[i].key, &b[pos], keysz);
			(*envs)[i].key[keysz] = '\0';
			(*envs)[i].keysz = keysz;
			(*envsz)++;
		} else
			free((*envs)[i].val);

		pos += keysz;

		/* Copy the value. */

		(*envs)[i].val = XMALLOC(valsz + 1);
		if (NULL == (*envs)[i].val)
			return(0);

		memcpy((*envs)[i].val, &b[pos], valsz);
		(*envs)[i].val[valsz] = '\0';
		(*envs)[i].valsz = valsz;

		pos += valsz;
#if 0
		fprintf(stderr, "%s: DEBUG: params: %s=%s\n", 
			__func__, (*envs)[i].key, (*envs)[i].val);
#endif
	}
	return(1);
}

/*
 * This is executed by the untrusted child for FastCGI setups.
 * Throughout, we follow the FastCGI specification, version 1.0, 29
 * April 1996.
 */
void
kworker_fcgi_child(int wfd, int work_ctl,
	const struct kvalid *keys, size_t keysz, 
	const char *const *mimes, size_t mimesz,
	unsigned int debugging)
{
	struct parms 	 pp;
	struct fcgi_hdr	*hdr, realhdr;
	struct fcgi_bgn	*bgn, realbgn;
	enum kcgi_err	 er;
	unsigned char	*buf, *sbuf;
	struct env	*envs;
	uint16_t	 rid;
	uint32_t	 cookie;
	size_t		 i, bsz, ssz, envsz;
	int		 rc, md5;
	enum kmethod	 meth;

	sbuf = NULL;
	ssz = 0;
	envsz = 0;
	envs = NULL;
	cookie = 0;
	bsz = 8;
	if (NULL == (buf = XMALLOC(bsz)))
		return;

	pp.fd = wfd;
	pp.keys = keys;
	pp.keysz = keysz;
	pp.mimes = mimes;
	pp.mimesz = mimesz;

	/*
	 * Loop over all incoming sequences to this particular slave.
	 * Sequences must consist of a single FastCGI session as defined
	 * in the FastCGI version 1.0 reference document.
	 */
	for (;;) {
		/* Clear all memory. */
		free(sbuf);
		sbuf = NULL;
		ssz = 0;
		for (i = 0; i < envsz; i++) {
			free(envs[i].key);
			free(envs[i].val);
		}
		free(envs);
		envs = NULL;
		envsz = 0;
		cookie = 0;

		/* 
		 * Begin by reading our magic cookie.
		 * This is emitted by the server at the start of our
		 * sequence.
		 * We'll reply with it on the control channel to
		 * indicate that we've finished our reads.
		 */

		if ((rc = fullread(work_ctl,
			 &cookie, sizeof(uint32_t), 1, &er)) < 0) {
			XWARNX("failed read FastCGI cookie");
			break;
		} else if (rc == 0)
			break;

		bgn = kworker_fcgi_begin
			(work_ctl, &realbgn, &buf, &bsz, &rid);
		if (NULL == bgn)
			break;

		/*
		 * Now read one or more parameters.
		 * We'll first read them all at once, then parse the
		 * headers in the content one by one.
		 */

		envsz = 0;
		for (;;) {
			hdr = kworker_fcgi_header
				(work_ctl, &realhdr, buf);
			if (NULL == hdr)
				break;
			if (rid != hdr->requestId) {
				XWARNX("unexpected FastCGI requestId");
				hdr = NULL;
				break;
			} else if (FCGI_PARAMS != hdr->type)
				break;
			if (kworker_fcgi_params
				(work_ctl, hdr, &buf, &bsz,
				 &envs, &envsz))
				continue;
			hdr = NULL;
			break;
		}
		if (NULL == hdr)
			break;

		if (FCGI_STDIN != hdr->type) {
			XWARNX("unexpected FastCGI header type");
			break;
		} else if (rid != hdr->requestId) {
			XWARNX("unexpected FastCGI requestId");
			break;
		}

		/*
		 * Lastly, we want to process the stdin content.
		 * These will end with a single zero-length record.
		 * Keep looping til we've flushed all input.
		 */

		for (;;) {
			rc = kworker_fcgi_stdin
				(work_ctl, hdr, &buf, &bsz, &sbuf, &ssz);
			if (rc <= 0)
				break;
			hdr = kworker_fcgi_header
				(work_ctl, &realhdr, buf);
			if (NULL == hdr)
				break;
			if (rid != hdr->requestId) {
				XWARNX("unexpected FastCGI requestId");
				hdr = NULL;
				break;
			} else if (FCGI_STDIN == hdr->type)
				continue;

			XWARNX("unexpected FastCGI header type");
			hdr = NULL;
			break;
		}
		if (rc < 0 || NULL == hdr) {
			XWARNX("not responding to FastCGI!?");
			break;
		}

		/* 
		 * Notify the control process that we've received all of
		 * our data by giving back the cookie and requestId.
		 */

		fullwrite(work_ctl, &cookie, sizeof(uint32_t));
		fullwrite(work_ctl, &rid, sizeof(uint16_t));

		/* 
		 * Now we can reply to our request.
		 * These are in a very specific order.
		 */

		kworker_child_env(envs, wfd, envsz);
		meth = kworker_child_method(envs, wfd, envsz);
		kworker_child_auth(envs, wfd, envsz);
		md5 = kworker_child_rawauth(envs, wfd, envsz);
		kworker_child_scheme(envs, wfd, envsz);
		kworker_child_remote(envs, wfd, envsz);
		kworker_child_path(envs, wfd, envsz);
		kworker_child_scriptname(envs, wfd, envsz);
		kworker_child_httphost(envs, wfd, envsz);
		kworker_child_port(envs, wfd, envsz);

		/* And now the message body itself. */

		assert(NULL != sbuf);
		kworker_child_body(envs, wfd, envsz, &pp, 
			meth, (char *)sbuf, ssz, debugging, md5);
		kworker_child_query(envs, wfd, envsz, &pp);
		kworker_child_cookies(envs, wfd, envsz, &pp);
		kworker_child_last(wfd);
	}

	for (i = 0; i < envsz; i++) {
		free(envs[i].key);
		free(envs[i].val);
	}
	free(sbuf);
	free(buf);
	free(envs);
}
