/*	$Id$ */
/*
 * Copyright (c) 2012, 2014 Kristaps Dzonsons <kristaps@bsd.lv>
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
#include <sys/mman.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "kcgi.h"

/* 10 MB upload limit for GET/POST. */
/* FIXME: pass this into http_parse(). */
#define	UPLOAD_LIMIT	 10485760

struct	kdata {
	enum kelem	 elems[128];
	size_t		 elemsz;
#ifdef	HAVE_ZLIB
	gzFile		 gz;
#endif
};

/*
 * For handling HTTP multipart forms.
 * This consists of data for a single multipart form entry.
 */
struct	hmime {
	char	 *disp; /* content disposition */
	char	 *name; /* name of form entry */
	size_t	  namesz; /* size of "name" string */
	char	 *file; /* whether a file was specified */
	char	 *ctype; /* content type */
	char	 *xcode; /* encoding type */
	char	 *bound; /* form entry boundary */
};

/*
 * A tag describes an HTML element.
 */
struct	tag {
	int		 flags; /* bit-field of flags */
#define	TAG_BLOCK	 1 /* block element (vs. inline) */
#define	TAG_ACLOSE	 2 /* auto-closing (e.g., INPUT) */
	const char	*name; /* name of element */
};

static	const uint16_t entities[KENTITY__MAX] = {
	0xE9, /* KENTITY_ACUTE */
	0x3E, /* KENTITY_GT */
	0x2190, /* KENTITY_LARR */
	0x3C, /* KENTITY_LT */
	0x2014, /* KENTITY_MDASH */
	0x2013, /* KENTITY_NDASH */
	0x2192, /* KENTITY_RARR */
};

static	const struct tag tags[KELEM__MAX] = {
	{ 0, "a" }, /* KELEM_A */
	{ 1, "blockquote" }, /* KELEM_BLOCKQUOTE */
	{ 1, "body" }, /* KELEM_BODY */
	{ 3, "br" }, /* KELEM_BR */
	{ 1, "caption" }, /* KELEM_CAPTION */
	{ 0, "code" }, /* KELEM_CODE */
	{ 3, "col" }, /* KELEM_COL */
	{ 1, "dd" }, /* KELEM_DD */
	{ 1, "div" }, /* KELEM_DIV */
	{ 1, "dl" }, /* KELEM_DL */
	{ 1, "dt" }, /* KELEM_DT */
	{ 1, "fieldset" }, /* KELEM_FIELDSET */
	{ 1, "form" }, /* KELEM_FORM */
	{ 1, "head" }, /* KELEM_HEAD */
	{ 1, "html" }, /* KELEM_HTML */
	{ 0, "i" }, /* KELEM_I */
	{ 2, "img" }, /* KELEM_IMG */
	{ 2, "input" }, /* KELEM_INPUT */
	{ 0, "label" }, /* KELEM_LABEL */
	{ 0, "legend" }, /* KELEM_LEGEND */
	{ 1, "li" }, /* KELEM_LI */
	{ 3, "link" }, /* KELEM_LINK */
	{ 3, "meta" }, /* KELEM_META */
	{ 0, "option" }, /* KELEM_OPTION */
	{ 1, "p" }, /* KELEM_P */
	{ 0, "q" }, /* KELEM_Q */
	{ 1, "select" }, /* KELEM_SELECT */
	{ 0, "span" }, /* KELEM_SPAN */
	{ 0, "strong" }, /* KELEM_STRONG */
	{ 1, "table" }, /* KELEM_TABLE */
	{ 1, "td" }, /* KELEM_TD */
	{ 1, "textarea" }, /* KELEM_TEXTAREA */
	{ 1, "th" }, /* KELEM_TH */
	{ 0, "title" }, /* KELEM_TITLE */
	{ 1, "tr" }, /* KELEM_TR */
	{ 1, "ul" }, /* KELEM_UL */
	{ 0, "var" }, /* KELEM_VAR */
};

const char *const kmimes[KMIME__MAX] = {
	"html", /* KMIME_HTML */
	"csv", /* KMIME_CSV */
	"png", /* KMIME_PNG */
};

const char *const kfields[KFIELD__MAX] = {
	"email",
	"password",
	"text",
	"number",
	"submit"
};

const char *const kmimetypes[KMIME__MAX] = {
	"text/html; charset=utf-8", /* KMIME_HTML */
	"text/csv", /* KMIME_CSV */
	"image/png", /* KMIME_PNG */
};

const char *const khttps[KHTTP__MAX] = {
	"200 OK",
	"303 See Other",
	"403 Forbidden",
	"404 Page Not Found",
	"409 Conflict",
	"415 Unsupported Media Type"
};

static	const char *const attrs[KATTR__MAX] = {
	"action", /* KATTR_ACTION */
	"checked", /* KATTR_CHECKED */
	"class", /* KATTR_CLASS */
	"clear", /* KATTR_CLEAR */
	"colspan", /* KATTR_COLSPAN */
	"content", /* KATTR_CONTENT */
	"disabled", /* KATTR_DISABLED */
	"enctype", /* KATTR_ENCTYPE */
	"for", /* KATTR_FOR */
	"href", /* KATTR_HREF */
	"http-equiv", /* KATTR_HTTP_EQUIV */
	"id", /* KATTR_ID */
	"max", /* KATTR_MAX */
	"method", /* KATTR_METHOD */
	"min", /* KATTR_MIN */
	"name", /* KATTR_NAME */
	"onclick", /* KATTR_ONCLICK */
	"rel", /* KATTR_REL */
	"rowspan", /* KATTR_ROWSPAN */
	"selected", /* KATTR_SELECTED */
	"span", /* KATTR_SPAN */
	"src", /* KATTR_SRC */
	"step", /* KATTR_STEP */
	"style", /* KATTR_STYLE */
	"target", /* KATTR_TARGET */
	"type", /* KATTR_TYPE */
	"value", /* KATTR_VALUE */
	"width", /* KATTR_WIDTH */
};

/* 
 * Name of executing CGI script.
 */
const char	*pname = NULL;

/*
 * Wrapper for printing data to the standard output.
 * This switches depending on compression support and whether our system
 * supports zlib compression at all.
 */
#ifdef HAVE_ZLIB
#define	KPRINTF(_req, ...) \
	do if (NULL != (_req)->kdata->gz) \
		gzprintf((_req)->kdata->gz, __VA_ARGS__); \
	else \
		printf(__VA_ARGS__); \
	while (0)
#else
#define	KPRINTF(_req, ...) printf(__VA_ARGS__)
#endif

/*
 * Wrapper for printing characters to standard output.
 * See KPRINTF().
 */
#ifdef HAVE_ZLIB
#define	KPUTCHAR(_req, _c) \
	do if (NULL != (_req)->kdata->gz) \
		gzputc((_req)->kdata->gz, (_c)); \
	else \
		putchar((_c)); \
	while (0)
#else
#define KPUTCHAR(_req, _c) putchar((_c))
#endif

/* 
 * Safe strdup(): don't return on memory failure.
 */
char *
kstrdup(const char *cp)
{
	char	*p;

	if (NULL != (p = strdup(cp)))
		return(p);

	perror(NULL);
	exit(EXIT_FAILURE);
}

/*
 * Safe realloc(): don't return on memory failure.
 */
void *
kxrealloc(void *pp, size_t sz)
{
	char	*p;

	if (NULL != (p = realloc(pp, sz)))
		return(p);

	perror(NULL);
	exit(EXIT_FAILURE);
}

/*
 * Safe realloc() with overflow-checking.
 */
void *
krealloc(void *pp, size_t nm, size_t sz)
{
	char	*p;

	if (nm && sz && SIZE_T_MAX / nm < sz) {
		errno = ENOMEM;
		perror(NULL);
		exit(EXIT_FAILURE);
	}

	if (NULL != (p = realloc(pp, nm * sz)))
		return(p);

	perror(NULL);
	exit(EXIT_FAILURE);
}

/*
 * Safe calloc(): don't return on exhaustion.
 */
void *
kcalloc(size_t nm, size_t sz)
{
	char	*p;

	if (NULL != (p = calloc(nm, sz)))
		return(p);

	perror(NULL);
	exit(EXIT_FAILURE);
}

/*
 * Safe malloc(): don't return on exhaustion.
 */
void *
kmalloc(size_t sz)
{
	char	*p;

	if (NULL != (p = malloc(sz)))
		return(p);

	perror(NULL);
	exit(EXIT_FAILURE);
}

#ifndef __OpenBSD__
/* OPENBSD ORIGINAL: lib/libc/stdlib/strtonum.c */
/* $OpenBSD: strtonum.c,v 1.6 2004/08/03 19:38:01 millert Exp $*/
/*
 * Copyright (c) 2004 Ted Unangst and Todd Miller
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#define INVALID 	1
#define TOOSMALL 	2
#define TOOLARGE 	3
static long long
strtonum(const char *numstr, long long minval, 
		long long maxval, const char **errstrp)
{
	long long ll = 0;
	char *ep;
	int error = 0;
	struct errval {
		const char *errstr;
		int err;
	} ev[4] = {
		{ NULL,		0 },
		{ "invalid",	EINVAL },
		{ "too small",	ERANGE },
		{ "too large",	ERANGE },
	};

	ev[0].err = errno;
	errno = 0;
	if (minval > maxval)
		error = INVALID;
	else {
		ll = strtoll(numstr, &ep, 10);
		if (numstr == ep || *ep != '\0')
			error = INVALID;
		else if ((ll == LLONG_MIN && errno == ERANGE) || 
			ll < minval)
			error = TOOSMALL;
		else if ((ll == LLONG_MAX && errno == ERANGE) || 
			ll > maxval)
			error = TOOLARGE;
	}
	if (errstrp != NULL)
		*errstrp = ev[error].errstr;
	errno = ev[error].err;
	if (error)
		ll = 0;

	return (ll);
}
#endif /*!__OpenBSD__*/

size_t
khtml_elemat(struct kreq *req)
{

	return(req->kdata->elemsz);
}

void
khtml_elem(struct kreq *req, enum kelem elem)
{

	khtml_attr(req, elem, KATTR__MAX);
}

void
khtml_input(struct kreq *req, size_t key)
{
	const char	*cp;
	char		 buf[64];

	cp = "";
	if (NULL != req->fieldmap[key])
		switch (req->fieldmap[key]->type) {
		case (KPAIR_DOUBLE):
			snprintf(buf, sizeof(buf), "%.2f",
				req->fieldmap[key]->parsed.d);
			cp = buf;
			break;
		case (KPAIR_INTEGER):
			snprintf(buf, sizeof(buf), "%lld",
				req->fieldmap[key]->parsed.i);
			cp = buf;
			break;
		case (KPAIR_STRING):
			cp = req->fieldmap[key]->parsed.s;
			break;
		default:
			abort();
			break;
		}

	assert(req->keys[key].field < KFIELD__MAX);
	khtml_attr(req, KELEM_INPUT,
		KATTR_TYPE, kfields[req->keys[key].field],
		KATTR_NAME, req->keys[key].name,
		KATTR_VALUE, cp,
		KATTR__MAX);
}

void
khtml_attr(struct kreq *req, enum kelem elem, ...)
{
	va_list		 ap;
	enum kattr	 at;
	struct kdata	*k = req->kdata;
	const char	*cp;

	KPRINTF(req, "<%s", tags[elem].name);

	va_start(ap, elem);
	while (KATTR__MAX != (at = va_arg(ap, enum kattr))) {
		cp = va_arg(ap, char *);
		assert(NULL != cp);
		KPRINTF(req, " %s=\"%s\"", attrs[at], cp);

	}
	va_end(ap);

	if (TAG_ACLOSE & tags[elem].flags)
		KPUTCHAR(req, '/');
	KPUTCHAR(req, '>');

	if (TAG_BLOCK & tags[elem].flags) 
		KPUTCHAR(req, '\n');

	if ( ! (TAG_ACLOSE & tags[elem].flags))
		k->elems[k->elemsz++] = elem;
	assert(k->elemsz < 128);
}

void
khtml_close(struct kreq *req, size_t sz)
{
	size_t		 i;
	struct kdata	*k = req->kdata;

	if (0 == sz)
		sz = k->elemsz;

	for (i = 0; i < sz; i++) {
		assert(k->elemsz > 0);
		k->elemsz--;
		KPRINTF(req, "</%s>", tags[k->elems[k->elemsz]].name);
		if (TAG_BLOCK & tags[k->elems[k->elemsz]].flags) 
			KPUTCHAR(req, '\n');
	}
}

void
khtml_closeto(struct kreq *req, size_t pos)
{

	assert(pos < req->kdata->elemsz);
	khtml_close(req, req->kdata->elemsz - pos);
}

void
khtml_decl(struct kreq *req)
{

	KPRINTF(req, "%s", "<!DOCTYPE html>\n");
}

/*
 * In-place HTTP-decode a string.  The standard explanation is that this
 * turns "%4e+foo" into "n foo" in the regular way.  This is done
 * in-place over the allocated string.
 */
static int
urldecode(char *p)
{
	char             hex[3];
	unsigned int	 c;

	hex[2] = '\0';

	for ( ; '\0' != *p; p++) {
		if ('%' == *p) {
			if ('\0' == (hex[0] = *(p + 1)))
				return(0);
			if ('\0' == (hex[1] = *(p + 2)))
				return(0);
			if (1 != sscanf(hex, "%x", &c))
				return(0);
			if ('\0' == c)
				return(0);

			*p = (char)c;
			memmove(p + 1, p + 3, strlen(p + 3) + 1);
		} else
			*p = /* LINTED */
				'+' == *p ? ' ' : *p;
	}

	*p = '\0';
	return(1);
}

/*
 * Parse out key-value pairs from an HTTP request variable.
 * This can be either a cookie or a POST/GET string.
 * This MUST be a non-binary (i.e., nil-terminated) string!
 */
static void
parse_pairs(struct kpair **kv, size_t *kvsz, char *p)
{
	char            *key, *val;
	size_t           sz;

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
			sz = strcspn(p, ";&");
			p += sz;
			if ('\0' != *p)
				*p++ = '\0';
		} else {
			/* No value--error. */
			p = key;
			sz = strcspn(p, ";&");
			p += sz;
			if ('\0' != *p)
				p++;
			continue;
		}

		if ('\0' == *key || '\0' == *val)
			continue;
		if (strlen(val) > UPLOAD_LIMIT)
			continue;
		if ( ! urldecode(key))
			break;
		if ( ! urldecode(val))
			break;
		*kv = krealloc(*kv, *kvsz + 1, sizeof(struct kpair));
		memset(&(*kv)[*kvsz], 0, sizeof(struct kpair));
		(*kv)[*kvsz].key = kstrdup(key);
		(*kv)[*kvsz].val = kstrdup(val);
		(*kv)[*kvsz].valsz = strlen(val);
		(*kvsz)++;
	}
}

static void
parse_urlenc(struct kpair **kv, size_t *kvsz)
{
	char		*p;
	ssize_t		 ssz;
	size_t		 sz;

	/*
	 * Read in chunks of 4096 from standard input.
	 * Make sure we have room for the nil terminator.
	 */
	p = kmalloc(4096 + 1);
	sz = 0;
	do {
		if ((ssz = read(STDIN_FILENO, p + sz, 4096)) <= 0)
			continue;
		sz += (size_t)ssz;
		/* Conservatively make sure we don't overflow. */
		assert(sz < SIZE_T_MAX - 4097);
		p = kxrealloc(p, sz + 4096 + 1);
	} while (ssz > 0);

	/* 
	 * If we have a nil terminator in the input sequence, we'll
	 * preemptively stop there in parse_pairs().
	 */
	p[sz] = '\0';
	parse_pairs(kv, kvsz, p);
	free(p);
}

/*
 * Use fgetln() to suck down lines from stdin.
 * The multipart format is line-based when in the "structure" part of
 * the form, which dictates control sequences and so on.
 * Note: we nil-terminate this line!
 * If it has a nil before the \n\r, subsequent functions will truncate
 * to that point.
 */
static char *
getformln(void)
{
	char		*line;
	size_t		 sz;

	/* Only handle \n\r terminated lines. */
	if (NULL == (line = fgetln(stdin, &sz)))
		return(NULL);
	else if (sz < 2)
		return(NULL);
	else if ('\n' != line[sz - 1])
		return(NULL);
	else if ('\r' != line[sz - 2])
		return(NULL);

	sz -= 2;
	line[sz] = '\0';
	return(line);
}

/*
 * Reset a particular hmime component.
 * We can get duplicates, so reallocate.
 */
static void
hmime_reset(char **dst, const char *src)
{

	free(*dst);
	*dst = kstrdup(src);
}

/*
 * Parse out a multipart instruction.
 * All of these operate on non-binary data, which is guaranteed by the
 * line parser.
 */
static int
hmime_parse(struct hmime *mime)
{
	char		*key, *val, *line;

	memset(mime, 0, sizeof(struct hmime));

	while (NULL != (line = getformln())) {
		if ('\0' == *line)
			return(1);
		key = line;
		if (NULL == (val = strchr(key, ':')))
			return(0);
		*val++ = '\0';
		while (isspace((int)*val))
			val++;

		line = NULL;
		if (NULL != val && NULL != (line = strchr(val, ';')))
			*line++ = '\0';

		/* White-list these control statements. */
		if (0 == strcasecmp(key, "content-transfer-encoding"))
			hmime_reset(&mime->xcode, val);
		else if (0 == strcasecmp(key, "content-disposition"))
			hmime_reset(&mime->disp, val);
		else if (0 == strcasecmp(key, "content-type"))
			hmime_reset(&mime->ctype, val);
		else
			return(0);

		/* These are sub-components. */
		while (NULL != (key = line)) {
			while (isspace((unsigned char)*key))
				key++;
			if ('\0' == *key)
				break;
			if (NULL == (val = strchr(key, '=')))
				return(0);
			*val++ = '\0';

			if ('"' == *val) {
				val++;
				line = strchr(val, '"');
				if (NULL == line)
					return(0);
				*line++ = '\0';
				if (';' == *line)
					line++;
			} else if (NULL != (line = strchr(val, ';')))
				*line++ = '\0';

			/* White-listed sub-commands. */
			if (0 == strcasecmp(key, "filename"))
				hmime_reset(&mime->file, val);
			else if (0 == strcasecmp(key, "name"))
				hmime_reset(&mime->name, val);
			else if (0 == strcasecmp(key, "boundary"))
				hmime_reset(&mime->bound, val);
			else
				return(0);
		}
	} 

	return(0);
}

/*
 * Get a binary line.
 * Only make sure that the end is a newline.
 * (Not a CRLF, which MIME dictates for transferral!)
 */
static char *
getbinln(size_t *sz)
{
	char		*line;

	if (NULL == (line = fgetln(stdin, sz)))
		return(NULL);
	else if ('\n' != line[*sz - 1])
		return(NULL);

	return(line);
}

/*
 * Test whether the line, not nil-terminated and with line size "sz",
 * matches the boundary b, not nil-terminated with size bsz.
 */
static int
boundary(const char *line, size_t sz, const char *b, size_t bsz)
{
	size_t		 extra;

	if (sz < bsz + 4)
		return(-1);
	if ('\n' != line[sz - 1] && '\r' != line[sz - 2])
		return(-1);

	sz -= 2;
	extra = sz - (bsz + 2);

	if ('-' != line[0] || '-' != line[1])
		return(-1);

	line += 2;
	if (memcmp(line, b, bsz))
		return(-1);

	line += bsz;
	if (2 == extra && 0 == memcmp(line, "--", 2))
		return(0);
	else if (0 == extra)
		return(1);

	return(-1);
}

/*
 * Parse a single value from a multipart form data entry.
 * Be careful: this can have nil-terminators and all sorts of ugliness.
 */
static int
form_parse(struct kpair **kv, size_t *kvsz, 
	const struct hmime *mime, const char *b, size_t bsz)
{
	char		*cp, *line;
	size_t		 sz, valsz;
	int		 rc;

	valsz = 0;
	cp = NULL;
	rc = -1;

	/*
	 * Download until we reach a hardcoded limit or the boundary is
	 * reached.  Or any other error occurs.
	 */
	while (NULL != (line = getbinln(&sz))) {
#ifdef DEBUG
		fprintf(stderr, "%s: Read %zu bytes "
			"(have %zu, max %d)\n", 
			__func__, sz, valsz, UPLOAD_LIMIT);
#endif
		if ((rc = boundary(line, sz, b, bsz)) >= 0)
			break;
		assert(sz > 0);
		assert(valsz < UPLOAD_LIMIT);
		/*
		 * Make sure we won't overflow the maximum buffer size
		 * even if that limit is close to the integer limit.
		 * In other words, avoid integer overflow.
		 */
		if (UPLOAD_LIMIT - valsz < sz) {
#ifdef DEBUG
			fprintf(stderr, "%s: Maximum exceeded "
				"(have %zu, wanted %zu, "
				"maximum %d)\n", __func__,
				maxsz, sz, UPLOAD_LIMIT);
#endif
			free(cp);
			return(-1);
		}
		cp = kxrealloc(cp, valsz + sz);
		memcpy(cp + valsz, line, sz);
		valsz += sz;
	}

#ifdef DEBUG
	fprintf(stderr, "%s: Exited read loop with "
		"%zu bytes\n", __func__, valsz);
#endif

	if (NULL == line || valsz < 2) {
#ifdef DEBUG
		fprintf(stderr, "%s: End of input stream "
			"while reading.\n", __func__);
#endif
		/* We didn't reach a boundary or have no CRLF. */
		free(cp);
		return(-1);
	} else if ('\n' != cp[valsz - 1] || '\r' != cp[valsz - 2]) {
#ifdef DEBUG
		fprintf(stderr, "%s: No CRLF "
			"while reading.\n", __func__);
#endif
		/* No proper CRLF ending. */
		free(cp);
		return(-1);
	}

	valsz -= 2;
	cp[valsz] = '\0';
	*kv = krealloc(*kv, *kvsz + 1, sizeof(struct kpair));
	memset(&(*kv)[*kvsz], 0, sizeof(struct kpair));
	assert(NULL != mime->name);
	(*kv)[*kvsz].key = kstrdup(mime->name);
	if (NULL != mime->ctype) 
		(*kv)[*kvsz].ctype = kstrdup(mime->ctype);
	if (NULL != mime->file) 
		(*kv)[*kvsz].file = kstrdup(mime->file);
	(*kv)[*kvsz].val = cp;
	(*kv)[*kvsz].valsz = valsz;
	(*kvsz)++;
	return(rc);
}

static void
hmime_free(struct hmime *mime)
{

	free(mime->disp);
	free(mime->name);
	free(mime->file);
	free(mime->ctype);
	free(mime->xcode);
	free(mime->bound);
	/* Help us segfault if re-used. */
	memset(mime, 0, sizeof(struct hmime));
}

/*
 * This parses a single form from a multi-form sequence.
 * The array "b" starts with a nil-terminated bounary sequence for the
 * given form.
 * Note that for multipart/mixed forms, we'll reenter this function for
 * the multipart.
 * FIXME: if we have multiple files come in, then we might not have a
 * "name" but the one that will inherit from the "top-level" multipart.
 */
static int
multiform_parse(struct kpair **kv, size_t *kvsz, const char *b)
{
	struct hmime	 mime;
	char		*line;
	int		 rc;
	size_t		 bsz;

	/* Stop if we're at the boundary (or problems). */
	if (NULL == b) 
		return(-1);
	else if (NULL == (line = getformln())) 
		return(-1);
	else if ('-' != line[0] || '-' != line[1])
		return(-1);
	else if (strcmp(line + 2, b))
		return(-1);

	rc = 1;
	bsz = strlen(b);

	while (rc > 0) {
		rc = -1;
		/* Try to parse the form MIME set. */
		if ( ! hmime_parse(&mime))
			break;

		/* Make sure we have a disposition and name. */
		if (NULL == mime.disp)
			break;
		else if (NULL == mime.name)
			break;
		/* FIXME: multipart/mixed might not have this. */
		else if (0 != strcmp(mime.disp, "form-data")) 
			break;

		if (NULL == mime.ctype || NULL != mime.file) {
			/* Read a binary file. */
			rc = form_parse(kv, kvsz, &mime, b, bsz);
			hmime_free(&mime);
			continue;
		} else if (0 != strcmp(mime.ctype, "multipart/mixed"))
			break;
		/*
		 * If we're a multipart/mixed file, we'll have multiple
		 * subcomponents with the given boundary.
		 */
		rc = multiform_parse(kv, kvsz, mime.bound);
		hmime_free(&mime);
	}

	hmime_free(&mime);
	return(rc);
}

/*
 * Parse an entire multi-part form.
 * This can consists of as many forms as possible.
 * Throughout the sequence, we're very careful with binary and
 * non-binary data.
 */
static void
parse_multi(struct kpair **kv, size_t *kvsz, char *line)
{
	size_t		 sz;

#ifdef DEBUG
	fprintf(stderr, "%s: starting\n", __func__);
#endif

	while (' ' == *line)
		line++;
	if (';' != *line++)
		return;
	while (' ' == *line)
		line++;

	/* Each form consists of a bounded sequence of lines. */
	sz = strlen("boundary=");
	if (strncmp(line, "boundary=", sz)) 
		return;
	line += sz;

	while (' ' == *line) 
		line++;
	/* Go ahead and parse the form... */
	multiform_parse(kv, kvsz, line);
}

/*
 * Parse a request from an HTTP request.
 * This consists of paths, suffixes, methods, and most importantly,
 * pasred query string, cookie, and form data.
 */
void
khttp_parse(struct kreq *req, 
	const struct kvalid *keys, size_t keysz,
	const char *const *pages, size_t pagesz,
	size_t defpage)
{
	char		*cp, *ep, *sub;
	enum kmime	 m;
	size_t		 p;
	size_t		 i, j;

	if (NULL == (pname = getenv("SCRIPT_NAME")))
		pname = "";

	memset(req, 0, sizeof(struct kreq));

	req->keys = keys;
	req->keysz = keysz;
	req->pages = pages;
	req->pagesz = pagesz;
	req->kdata = kcalloc(1, sizeof(struct kdata));
	req->cookiemap = kcalloc(keysz, sizeof(struct kpair *));
	req->cookienmap = kcalloc(keysz, sizeof(struct kpair *));
	req->fieldmap = kcalloc(keysz, sizeof(struct kpair *));
	req->fieldnmap = kcalloc(keysz, sizeof(struct kpair *));

	sub = NULL;
	p = defpage;
	m = KMIME_HTML;

	/*
	 * First, parse the first path element (the page we want to
	 * access), subsequent path information, and the file suffix.
	 * We convert suffix and path element into the respective enum's
	 * inline.
	 */

	req->method = KMETHOD_GET;
	if (NULL != (cp = getenv("REQUEST_METHOD")) &&
			0 == strcasecmp(cp, "post"))
		req->method = KMETHOD_POST;

	if (NULL != (cp = getenv("PATH_INFO")) && '/' == *cp)
		cp++;

	if (NULL != cp && '\0' != *cp) {
		ep = cp + strlen(cp) - 1;
		while (ep > cp && '/' != *ep && '.' != *ep)
			ep--;

		if ('.' == *ep)
			for (*ep++ = '\0', m = 0; m < KMIME__MAX; m++)
				if (0 == strcasecmp(kmimes[m], ep))
					break;

		if (NULL != (sub = strchr(cp, '/')))
			*sub++ = '\0';

		for (p = 0; p < pagesz; p++)
			if (0 == strcasecmp(pages[p], cp))
				break;
	}

	req->mime = m;
	req->page = p;
	if (NULL != sub)
		req->path = kstrdup(sub);

	/*
	 * If a CONTENT_TYPE has been specified (i.e., POST or GET has
	 * been set -- we don't care which), then switch on that type
	 * for parsing out key value pairs.
	 */
	if (NULL != (cp = getenv("CONTENT_TYPE"))) {
		if (0 == strcmp(cp, "application/x-www-form-urlencoded"))
			parse_urlenc(&req->fields, &req->fieldsz);
		else if (0 == strncmp(cp, "multipart/form-data", 19)) 
			parse_multi(&req->fields, &req->fieldsz, cp + 19);
	}

	/*
	 * Even POST requests are allowed to have QUERY_STRING elements,
	 * so parse those out now.
	 * Note: both QUERY_STRING and CONTENT_TYPE fields share the
	 * same field space.
	 * Since this is a getenv(), we know the returned value is
	 * nil-terminated.
	 */
	if (NULL != (cp = getenv("QUERY_STRING")))
		parse_pairs(&req->fields, &req->fieldsz, cp);

	/*
	 * Cookies come last.
	 * These use the same syntax as QUERY_STRING elements, but don't
	 * share the same namespace (just as a means to differentiate
	 * the same names).
	 * Since this is a getenv(), we know the returned value is
	 * nil-terminated.
	 */
	if (NULL != (cp = getenv("HTTP_COOKIE")))
		parse_pairs(&req->cookies, &req->cookiesz, cp);

	/*
	 * Run through all fields and sort them into named buckets.
	 * This will let us do constant-time lookups within the
	 * application itself.  Nice.
	 */
	for (i = 0; i < keysz; i++) {
		for (j = 0; j < req->fieldsz; j++) {
			if (strcmp(req->fields[j].key, keys[i].name))
				continue;
			if (NULL != keys[i].valid &&
				! (*keys[i].valid)(&req->fields[j])) {
				req->fields[j].type = KPAIR__MAX;
				req->fields[j].next = req->fieldnmap[i];
				req->fieldnmap[i] = &req->fields[j];
				continue;
			}
			assert(NULL == keys[i].valid ||
				KPAIR__MAX != req->fields[j].type);
			req->fields[j].next = req->fieldmap[i];
			req->fieldmap[i] = &req->fields[j];
		}
		for (j = 0; j < req->cookiesz; j++) {
			if (strcmp(req->cookies[j].key, keys[i].name))
				continue;
			if (NULL != keys[i].valid &&
				! keys[i].valid(&req->cookies[j])) {
				req->cookies[j].type = KPAIR__MAX;
				req->cookies[j].next = req->cookienmap[i];
				req->cookienmap[i] = &req->cookies[j];
				continue;
			}
			assert(NULL == keys[i].valid ||
				KPAIR__MAX != req->cookies[j].type);
			req->cookies[j].next = req->cookiemap[i];
			req->cookiemap[i] = &req->cookies[j];
		}
	}
}

static void
kpair_free(struct kpair *p, size_t sz)
{
	size_t		 i;

	for (i = 0; i < sz; i++) {
		free(p[i].key);
		free(p[i].val);
		free(p[i].file);
		free(p[i].ctype);
	}
	free(p);
}

void
khttp_free(struct kreq *req)
{

#ifdef HAVE_ZLIB
	if (NULL != req->kdata->gz)
		gzclose(req->kdata->gz);
#endif
	kpair_free(req->cookies, req->cookiesz);
	kpair_free(req->fields, req->fieldsz);
	free(req->path);
	free(req->cookiemap);
	free(req->cookienmap);
	free(req->fieldmap);
	free(req->fieldnmap);
	free(req->kdata);
}

void
khtml_entity(struct kreq *req, enum kentity entity)
{

	assert(entity < KENTITY__MAX);
	khtml_ncr(req, entities[entity]);
}

void
khtml_ncr(struct kreq *req, uint16_t ncr)
{

	KPRINTF(req, "&#x%" PRIu16 ";", ncr);
}

void
khttp_head(struct kreq *req, const char *key, const char *fmt, ...)
{
	va_list	 ap;

	printf("%s: ", key);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	printf("\r\n");
	va_end(ap);
}

void
khttp_body(struct kreq *req)
{
#ifdef HAVE_ZLIB
	const char	*cp;
#endif

	/*
	 * If gzip is an accepted encoding, then create the "gz" stream
	 * that will be used for all subsequent I/O operations.
	 */
#ifdef HAVE_ZLIB
	if (NULL != (cp = getenv("HTTP_ACCEPT_ENCODING")) &&
			NULL != strstr(cp, "gzip")) {
		req->kdata->gz = gzdopen(STDOUT_FILENO, "w");
		if (NULL == req->kdata->gz) {
			perror(NULL);
			exit(EXIT_FAILURE);
		}
		khttp_head(req, "Content-Encoding", "%s", "gzip");
	} 
#endif
	/*
	 * XXX: newer versions of zlib have a "T" transparent mode that
	 * one can add to gzdopen() that allows using the gz without any
	 * compression.
	 * Unfortunately, that's not guaranteed on all systems, so we
	 * must do without it.
	 */

	fputs("\r\n", stdout);
	fflush(stdout);
}

void
khtml_text(struct kreq *req, const char *cp)
{

	for ( ; NULL != cp && '\0' != *cp; cp++)
		switch (*cp) {
		case ('>'):
			khtml_entity(req, KENTITY_GT);
			break;
		case ('<'):
			khtml_entity(req, KENTITY_LT);
			break;
		default:
			KPUTCHAR(req, *cp);
			break;
		}
}

/*
 * Trim leading and trailing whitespace from a word.
 */
static char *
trim(char *val)
{
	char		*cp;

	if ('\0' == *val)
		return(val);

	cp = val + strlen(val) - 1;
	while (cp > val && isspace((unsigned char)*cp))
		*cp-- = '\0';

	cp = val;
	while (isspace((unsigned char)*cp))
		cp++;

	return(cp);
}

/*
 * Simple email address validation: this is NOT according to the spec,
 * but a simple heuristic look at the address.
 * Note that this lowercases the mail address.
 */
static char *
valid_email(char *p)
{
	char		*domain, *cp, *start;
	size_t		 i, sz;

	cp = start = trim(p);

	if ((sz = strlen(cp)) < 5 || sz > 254)
		return(NULL);
	if (NULL == (domain = strchr(cp, '@')))
		return(NULL);
	if ((sz = domain - cp) < 1 || sz > 64)
		return(NULL);

	for (i = 0; i < sz; i++) {
		if (isalnum((unsigned char)cp[i]))
			continue;
		if (NULL == strchr("!#$%&'*+-/=?^_`{|}~.", cp[i]))
			return(NULL);
	}

	assert('@' == cp[i]);
	cp = &cp[++i];
	if ((sz = strlen(cp)) < 4 || sz > 254)
		return(NULL);

	for (i = 0; i < sz; i++) 
		if ( ! isalnum((unsigned char)cp[i]))
			if (NULL == strchr("-.", cp[i]))
				return(NULL);

	for (cp = start; '\0' != *cp; cp++)
		*cp = tolower((unsigned char)*cp);

	return(start);
}

int
kvalid_string(struct kpair *p)
{

	/*
	 * To check if we're a valid string, simply make sure that the
	 * nil pointer is where we expect it to be.
	 */
	if (strlen(p->val) != p->valsz)
		return(0);
	p->type = KPAIR_STRING;
	p->parsed.s = p->val;
	return(1);
}

int
kvalid_email(struct kpair *p)
{

	if ( ! kvalid_string(p))
		return(0);
	return(NULL != (p->parsed.s = valid_email(p->val)));
}

int
kvalid_udouble(struct kpair *p)
{

	if ( ! kvalid_double(p))
		return(0);
	p->type = KPAIR_DOUBLE;
	return(isnormal(p->parsed.d) && p->parsed.d > 0.0);
}

int
kvalid_double(struct kpair *p)
{
	char		*ep;
	double		 lval;
	const char	*cp;

	if ( ! kvalid_string(p))
		return(0);

	if (NULL != (cp = strrchr(p->val, '.')))
		if (strlen(cp + 1) > 2) 
			return(0);

	errno = 0;
	lval = strtod(p->val, &ep);
	if (p->val[0] == '\0' || *ep != '\0')
		return(0);
	if (errno == ERANGE && (lval == HUGE_VAL || lval == -HUGE_VAL))
		return(0);
	p->parsed.d = lval;
	p->type = KPAIR_DOUBLE;
	return(1);
}

int
kvalid_int(struct kpair *p)
{
	const char	*ep;

	if ( ! kvalid_string(p))
		return(0);
	p->parsed.i = strtonum
		(trim(p->val), INT64_MIN, INT64_MAX, &ep);
	p->type = KPAIR_INTEGER;
	return(NULL == ep);
}

int
kvalid_uint(struct kpair *p)
{
	const char	*ep;

	p->parsed.i = strtonum(trim(p->val), 1, INT64_MAX, &ep);
	p->type = KPAIR_INTEGER;
	return(NULL == ep);
}

/*
 * There are all sorts of ways to make this faster and more efficient.
 * For now, do it the easily-auditable way.
 */
int
khtml_template(struct kreq *req, 
	const struct ktemplate *t, const char *fname)
{
	struct stat 	 st;
	char		*buf;
	size_t		 sz, i, j, len, start, end;
	int		 fd, rc;

	if (-1 == (fd = open(fname, O_RDONLY, 0)))
		return(0);

	if (-1 == fstat(fd, &st)) {
		close(fd);
		return(0);
	} else if (st.st_size >= (1U << 31)) {
		close(fd);
		return(0);
	} else if (0 == st.st_size) {
		close(fd);
		return(1);
	}

	sz = (size_t)st.st_size;
	buf = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);

	if (NULL == buf) {
		close(fd);
		return(0);
	}

	rc = 0;

	for (i = 0; i < sz - 1; i++) {
		/* Look for the starting "@@" marker. */
		if ('@' != buf[i]) {
			KPUTCHAR(req, buf[i]);
			continue;
		} else if ('@' != buf[i + 1]) {
			KPUTCHAR(req, buf[i]);
			continue;
		} 

		/* Seek to find the end "@@" marker. */
		start = i + 2;
		for (end = start + 2; end < sz - 1; end++)
			if ('@' == buf[end] && '@' == buf[end + 1])
				break;

		/* Continue printing if not found of 0-length. */
		if (end == sz - 1 || end == start) {
			KPUTCHAR(req, buf[i]);
			continue;
		}

		/* Look for a matching key. */
		for (j = 0; j < t->keysz; j++) {
			len = strlen(t->key[j]);
			if (len != end - start)
				continue;
			else if (memcmp(&buf[start], t->key[j], len))
				continue;
			if ( ! (*t->cb)(j, t->arg))
				goto out;
			break;
		}

		/* Didn't find it... */
		if (j == t->keysz)
			KPUTCHAR(req, buf[i]);
		else
			i = end + 1;
	}

	if (i < sz)
		KPUTCHAR(req, buf[i]);

	rc = 1;
out:
	munmap(buf, sz);
	close(fd);
	return(rc);
}
