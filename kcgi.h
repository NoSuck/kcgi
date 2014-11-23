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
#ifndef KCGI_H
#define KCGI_H

/*
 * All of the public functions, variables, and structures in this header
 * file are described in kcgi(3).
 * If you don't see something in kcgi(3) and see it here instead, it
 * shouldn't be used.
 */

enum	khttp {
	KHTTP_100,
	KHTTP_101,
	KHTTP_103,
	KHTTP_200,
	KHTTP_201,
	KHTTP_202,
	KHTTP_203,
	KHTTP_204,
	KHTTP_205,
	KHTTP_206,
	KHTTP_300,
	KHTTP_301,
	KHTTP_302,
	KHTTP_303,
	KHTTP_304,
	KHTTP_306,
	KHTTP_307,
	KHTTP_308,
	KHTTP_400,
	KHTTP_401,
	KHTTP_402,
	KHTTP_403,
	KHTTP_404,
	KHTTP_405,
	KHTTP_406,
	KHTTP_407,
	KHTTP_408,
	KHTTP_409,
	KHTTP_410,
	KHTTP_411,
	KHTTP_412,
	KHTTP_413,
	KHTTP_414,
	KHTTP_415,
	KHTTP_416,
	KHTTP_417,
	KHTTP_500,
	KHTTP_501,
	KHTTP_502,
	KHTTP_503,
	KHTTP_504,
	KHTTP_505,
	KHTTP_511,
	KHTTP__MAX
};

enum	kresp {
	KRESP_ACCESS_CONTROL_ALLOW_ORIGIN,
	KRESP_ACCEPT_RANGES,
	KRESP_AGE,
	KRESP_ALLOW,
	KRESP_CACHE_CONTROL,
	KRESP_CONNECTION,
	KRESP_CONTENT_ENCODING,
	KRESP_CONTENT_LANGUAGE,
	KRESP_CONTENT_LENGTH,
	KRESP_CONTENT_LOCATION,
	KRESP_CONTENT_MD5,
	KRESP_CONTENT_DISPOSITION,
	KRESP_CONTENT_RANGE,
	KRESP_CONTENT_TYPE,
	KRESP_DATE,
	KRESP_ETAG,
	KRESP_EXPIRES,
	KRESP_LAST_MODIFIED,
	KRESP_LINK,
	KRESP_LOCATION,
	KRESP_P3P,
	KRESP_PRAGMA,
	KRESP_PROXY_AUTHENTICATE,
	KRESP_REFRESH,
	KRESP_RETRY_AFTER,
	KRESP_SERVER,
	KRESP_SET_COOKIE,
	KRESP_STATUS,
	KRESP_STRICT_TRANSPORT_SECURITY,
	KRESP_TRAILER,
	KRESP_TRANSFER_ENCODING,
	KRESP_UPGRADE,
	KRESP_VARY,
	KRESP_VIA,
	KRESP_WARNING,
	KRESP_WWW_AUTHENTICATE,
	KRESP_X_FRAME_OPTIONS,
	KRESP__MAX
};

enum	kattrx {
	KATTRX_STRING,
	KATTRX_INT,
	KATTRX_DOUBLE
};

enum	kmethod {
	KMETHOD_POST,
	KMETHOD_GET
};

enum	kpairtype {
	KPAIR_INTEGER,
	KPAIR_STRING,
	KPAIR_DOUBLE,
	KPAIR__MAX
};

enum	kscheme {
	KSCHEME_AAA,
	KSCHEME_AAAS,
	KSCHEME_ABOUT,
	KSCHEME_ACAP,
	KSCHEME_ACCT,
	KSCHEME_CAP,
	KSCHEME_CID,
	KSCHEME_COAP,
	KSCHEME_COAPS,
	KSCHEME_CRID,
	KSCHEME_DATA,
	KSCHEME_DAV,
	KSCHEME_DICT,
	KSCHEME_DNS,
	KSCHEME_FILE,
	KSCHEME_FTP,
	KSCHEME_GEO,
	KSCHEME_GO,
	KSCHEME_GOPHER,
	KSCHEME_H323,
	KSCHEME_HTTP,
	KSCHEME_HTTPS,
	KSCHEME_IAX,
	KSCHEME_ICAP,
	KSCHEME_IM,
	KSCHEME_IMAP,
	KSCHEME_INFO,
	KSCHEME_IPP,
	KSCHEME_IRIS,
	KSCHEME_IRIS_BEEP,
	KSCHEME_IRIS_XPC,
	KSCHEME_IRIS_XPCS,
	KSCHEME_IRIS_LWZ,
	KSCHEME_JABBER,
	KSCHEME_LDAP,
	KSCHEME_MAILTO,
	KSCHEME_MID,
	KSCHEME_MSRP,
	KSCHEME_MSRPS,
	KSCHEME_MTQP,
	KSCHEME_MUPDATE,
	KSCHEME_NEWS,
	KSCHEME_NFS,
	KSCHEME_NI,
	KSCHEME_NIH,
	KSCHEME_NNTP,
	KSCHEME_OPAQUELOCKTOKEN,
	KSCHEME_POP,
	KSCHEME_PRES,
	KSCHEME_RELOAD,
	KSCHEME_RTSP,
	KSCHEME_RTSPS,
	KSCHEME_RTSPU,
	KSCHEME_SERVICE,
	KSCHEME_SESSION,
	KSCHEME_SHTTP,
	KSCHEME_SIEVE,
	KSCHEME_SIP,
	KSCHEME_SIPS,
	KSCHEME_SMS,
	KSCHEME_SNMP,
	KSCHEME_SOAP_BEEP,
	KSCHEME_SOAP_BEEPS,
	KSCHEME_STUN,
	KSCHEME_STUNS,
	KSCHEME_TAG,
	KSCHEME_TEL,
	KSCHEME_TELNET,
	KSCHEME_TFTP,
	KSCHEME_THISMESSAGE,
	KSCHEME_TN3270,
	KSCHEME_TIP,
	KSCHEME_TURN,
	KSCHEME_TURNS,
	KSCHEME_TV,
	KSCHEME_URN,
	KSCHEME_VEMMI,
	KSCHEME_WS,
	KSCHEME_WSS,
	KSCHEME_XCON,
	KSCHEME_XCON_USERID,
	KSCHEME_XMLRPC_BEEP,
	KSCHEME_XMLRPC_BEEPS,
	KSCHEME_XMPP,
	KSCHEME_Z39_50R,
	KSCHEME_Z39_50S,
	KSCHEME__MAX
};

enum	kmime {
	KMIME_APP_JAVASCRIPT,
	KMIME_APP_JSON,
	KMIME_IMAGE_GIF,
	KMIME_IMAGE_JPEG,
	KMIME_IMAGE_PNG,
	KMIME_IMAGE_SVG_XML,
	KMIME_TEXT_CSS,
	KMIME_TEXT_CSV,
	KMIME_TEXT_HTML,
	KMIME_TEXT_PLAIN,
	KMIME__MAX
};

struct	kmimemap {
	const char	*name;
	size_t		 mime;
};

enum	kpairstate {
	KPAIR_UNCHECKED = 0,
	KPAIR_VALID,
	KPAIR_INVALID
};

struct	kpair {
	char		*key; /* key name */
	size_t		 keypos; /* bucket (if assigned) */
	char		*val; /*  key value */
	size_t		 valsz; /* length of "val" */
	char		*file; /* content filename (or NULL) */
	char		*ctype; /* content type (or NULL) */
	size_t		 ctypepos; /* content type index */
	char		*xcode; /* content xfer encoding (or NULL) */
	struct kpair	*next; /* next in map entry */
	enum kpairstate	 state; /* parse state */
	enum kpairtype	 type; /* if parsed, the parse type */
	union parsed {
		int64_t i; /* validated integer */
		const char *s; /* validated string */
		double d; /* validated decimal */
	} parsed;
};

struct	kreq; /* forward declaration */

struct	kvalid {
	int		(*valid)(struct kpair *kp);
	const char	 *name;
};

enum	kauth {
	KAUTH_NONE = 0,
	KAUTH_BASIC,
	KAUTH_DIGEST,
	KAUTH_UNKNOWN
};

struct	kdata;

struct	kreq {
	enum kmethod		  method;
	enum kauth		  auth;
	struct kpair		 *cookies;
	size_t			  cookiesz;
	struct kpair		**cookiemap;
	struct kpair		**cookienmap;
	struct kpair		 *fields;
	struct kpair		**fieldmap;
	struct kpair		**fieldnmap;
	size_t			  fieldsz;
	size_t			  mime;
	size_t			  page;
	char			 *path;
	char			 *suffix;
	char			 *fullpath;
	char			 *remote;
	char			 *host;
	uint16_t		  port;
	struct kdata		 *kdata;
	const struct kvalid	 *keys;
	size_t			  keysz;
	char			 *pname;
	void			 *arg; 
};

struct	ktemplate {
	const char *const	 *key;
	size_t		 	  keysz;
	void		 	 *arg;
	int		 	(*cb)(size_t key, void *arg);
};

__BEGIN_DECLS

void		 khttp_body(struct kreq *req);
void		 khttp_free(struct kreq *req);
void		 khttp_child_free(struct kreq *req);
void		 khttp_head(struct kreq *req, const char *key, 
			const char *fmt, ...)
			__attribute__((format(printf, 3, 4)));
int		 khttp_parse(struct kreq *req, 
			const struct kvalid *keys, size_t keymax,
			const char *const *pages, size_t pagemax,
			size_t defpage);
int		 khttp_parsex(struct kreq *req, 
			const struct kmimemap *suffixes, 
			const char *const *mimes, size_t mimemax,
			const struct kvalid *keys, size_t keymax,
			const char *const *pages, size_t pagemax,
			size_t defpage, void *arg,
			void (*argfree)(void *arg));
void		 khttp_putc(struct kreq *req, int c);
void		 khttp_puts(struct kreq *req, const char *cp);
int		 khttp_template(struct kreq *req, 
			const struct ktemplate *t, 
			const char *fname);
int		 khttp_template_buf(struct kreq *req, 
			const struct ktemplate *t, 
			const char *buf, size_t sz);
void		 khttp_write(struct kreq *req, const void *buf, 
			size_t sz);

int		 kvalid_date(struct kpair *);
int		 kvalid_double(struct kpair *);
int		 kvalid_email(struct kpair *);
int		 kvalid_int(struct kpair *);
int		 kvalid_string(struct kpair *);
int		 kvalid_stringne(struct kpair *);
int		 kvalid_udouble(struct kpair *);
int		 kvalid_uint(struct kpair *);

char		*kutil_urlabs(enum kscheme scheme, const char *host, 
			uint16_t port, const char *path);
char		*kutil_urlpart(struct kreq *req, const char *path,
			const char *mime, const char *page, ...);
char		*kutil_urlpartx(struct kreq *req, const char *path,
			const char *mime, const char *page, ...);
char		*kutil_urlencode(const char *cp);
void		 kutil_invalidate(struct kreq *req,
			struct kpair *pair);

int		 kasprintf(char **p, const char *fmt, ...);
void		*kcalloc(size_t nm, size_t sz);
void		*kmalloc(size_t sz);
void		*krealloc(void *p, size_t sz);
void		*kreallocarray(void *p, size_t nm, size_t sz);
char		*kstrdup(const char *cp);

extern const char *const	 kmimetypes[KMIME__MAX];
extern const char *const	 khttps[KHTTP__MAX];
extern const char *const	 kresps[KRESP__MAX];
extern const struct kmimemap	 ksuffixmap[];
extern const char *const	 ksuffixes[KMIME__MAX];

__END_DECLS

#endif /*!KCGI_H*/
