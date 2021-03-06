/*
 * ws protocol handler plugin for "generic sessions"
 *
 * Copyright (C) 2010-2016 Andy Green <andy@warmcat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation:
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#define LWS_DLL
#define LWS_INTERNAL
#include "../lib/libwebsockets.h"

#include <sqlite3.h>
#include <string.h>

#define LWSGS_EMAIL_CONTENT_SIZE 16384
#define LWSGS_VERIFIED_ACCEPTED 100

/* SHA-1 binary and hexified versions */
typedef struct { unsigned char bin[20]; } lwsgw_hash_bin;
typedef struct { char id[41]; } lwsgw_hash;

enum lwsgs_auth_bits {
	LWSGS_AUTH_LOGGED_IN = 1,
	LWSGS_AUTH_ADMIN = 2,
	LWSGS_AUTH_VERIFIED = 4,
	LWSGS_AUTH_FORGOT_FLOW = 8,
};

struct lwsgs_user {
	char username[32];
	char ip[16];
	lwsgw_hash pwhash;
	lwsgw_hash pwsalt;
	lwsgw_hash token;
	time_t created;
	time_t last_forgot_validated;
	char email[100];
	int verified;
};

struct per_vhost_data__generic_sessions {
	struct lws_email email;
	struct lws_context *context;
	char session_db[256];
	char admin_user[32];
	char confounder[32];
	char email_contact_person[128];
	char email_title[128];
	char email_template[128];
	char email_confirm_url[128];
	lwsgw_hash admin_password_sha1;
	sqlite3 *pdb;
	int timeout_idle_secs;
	int timeout_absolute_secs;
	int timeout_anon_absolute_secs;
	int timeout_email_secs;
	time_t last_session_expire;
	struct lwsgs_user u;
};

static const char * const param_names[] = {
	"username",
	"password",
	"password2",
	"email",
	"register",
	"good",
	"bad",
	"reg-good",
	"reg-bad",
	"admin",
	"forgot",
	"forgot-good",
	"forgot-bad",
	"forgot-post-good",
	"forgot-post-bad",
	"change",
	"curpw"
};

enum {
	FGS_USERNAME,
	FGS_PASSWORD,
	FGS_PASSWORD2,
	FGS_EMAIL,
	FGS_REGISTER,
	FGS_GOOD,
	FGS_BAD,
	FGS_REG_GOOD,
	FGS_REG_BAD,
	FGS_ADMIN,
	FGS_FORGOT,
	FGS_FORGOT_GOOD,
	FGS_FORGOT_BAD,
	FGS_FORGOT_POST_GOOD,
	FGS_FORGOT_POST_BAD,
	FGS_CHANGE,
	FGS_CURPW,
};

struct per_session_data__generic_sessions {
	struct lws_urldecode_stateful_param_array *spa;
	lwsgw_hash login_session;
	lwsgw_hash delete_session;
	unsigned int login_expires;
	char onward[256];
	char result[500 + LWS_PRE];
	char urldec[500 + LWS_PRE];
	int result_len;
	char *start;
	char swallow[16];
	char ip[46];
	int pos;
	int spos;

	unsigned int logging_out:1;
};

static void
sha1_to_lwsgw_hash(unsigned char *hash, lwsgw_hash *shash)
{
	static const char *hex = "0123456789abcdef";
	char *p = shash->id;
	int n;

	for (n = 0; n < 20; n++) {
		*p++ = hex[hash[n] >> 4];
		*p++ = hex[hash[n] & 15];
	}

	*p = '\0';
}

static unsigned int
lwsgs_now_secs(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return tv.tv_sec;
}

static int
lwsgw_check_admin(struct per_vhost_data__generic_sessions *vhd,
		  const char *username, const char *password)
{
	lwsgw_hash_bin hash_bin;
	lwsgw_hash pw_hash;

	if (strcmp(vhd->admin_user, username))
		return 0;

	lws_SHA1((unsigned char *)password, strlen(password), hash_bin.bin);
	sha1_to_lwsgw_hash(hash_bin.bin, &pw_hash);

	return !strcmp(vhd->admin_password_sha1.id, pw_hash.id);
}

/*
 * secure cookie: it can only be passed over https where it cannot be
 *		  snooped in transit
 * HttpOnly:	  it can only be accessed via http[s] transport, it cannot be
 *		  gotten at by JS
 */
static void
lwsgw_cookie_from_session(lwsgw_hash *sid, time_t expires,
			  char **p, char *end)
{
	struct tm *tm = gmtime(&expires);
	time_t n = lwsgs_now_secs();

	*p += snprintf(*p, end - *p, "id=%s;Expires=", sid->id);
#ifdef WIN32
	*p += strftime(*p, end - *p, "%Y %H:%M %Z", tm);
#else
	*p += strftime(*p, end - *p, "%F %H:%M %Z", tm);
#endif
	*p += snprintf(*p, end - *p, ";path=/");
	*p += snprintf(*p, end - *p, ";Max-Age=%lu", (unsigned long)(expires - n));
//	*p += snprintf(*p, end - *p, ";secure");
	*p += snprintf(*p, end - *p, ";HttpOnly");
}

static int
lwsgw_expire_old_sessions(struct per_vhost_data__generic_sessions *vhd)
{
	time_t n = lwsgs_now_secs();
	char s[200];

	if (n - vhd->last_session_expire < 5)
		return 0;

	vhd->last_session_expire = n;

	snprintf(s, sizeof(s) - 1,
		 "delete from sessions where "
		 "expire <= %lu;", (unsigned long)n);

	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("Unable to expire sessions: %s\n",
			 sqlite3_errmsg(vhd->pdb));
		return 1;
	}

	return 0;
}

static int
lwsgw_update_session(struct per_vhost_data__generic_sessions *vhd,
		     lwsgw_hash *hash, const char *user)
{
	time_t n = lwsgs_now_secs();
	char s[200], esc[50], esc1[50];

	if (user[0])
		n += vhd->timeout_absolute_secs;
	else
		n += vhd->timeout_anon_absolute_secs;

	snprintf(s, sizeof(s) - 1,
		 "update sessions set expire=%lu,username='%s' where name='%s';",
		 (unsigned long)n,
		 lws_sql_purify(esc, user, sizeof(esc)),
		 lws_sql_purify(esc1, hash->id, sizeof(esc1)));

	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("Unable to update session: %s\n",
			 sqlite3_errmsg(vhd->pdb));
		return 1;
	}

	return 0;
}

static int
lwsgw_session_from_cookie(const char *cookie, lwsgw_hash *sid)
{
	const char *p = cookie;
	int n;

	while (*p) {
		if (p[0] == 'i' && p[1] == 'd' && p[2] == '=') {
			p += 3;
			break;
		}
		p++;
	}
	if (!*p) {
		lwsl_info("no id= in cookie\n");
		return 1;
	}

	for (n = 0; n < sizeof(sid->id) - 1 && *p; n++) {
		/* our SID we issue only has these chars */
		if ((*p >= '0' && *p <= '9') ||
		    (*p >= 'a' && *p <= 'f'))
			sid->id[n] = *p++;
		else {
			lwsl_info("bad chars in cookie id %c\n", *p);
			return 1;
		}
	}

	if (n < sizeof(sid->id) - 1) {
		lwsl_info("cookie id too short\n");
		return 1;
	}

	sid->id[sizeof(sid->id) - 1] = '\0';

	return 0;
}

static int
lwsgs_get_sid_from_wsi(struct lws *wsi, lwsgw_hash *sid)
{
	char cookie[1024];

	/* fail it on no cookie */
	if (!lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_COOKIE)) {
		lwsl_info("%s: no cookie\n", __func__);
		return 1;
	}
	if (lws_hdr_copy(wsi, cookie, sizeof cookie, WSI_TOKEN_HTTP_COOKIE) < 0) {
		lwsl_info("cookie copy failed\n");
		return 1;
	}
	/* extract the sid from the cookie */
	if (lwsgw_session_from_cookie(cookie, sid)) {
		lwsl_info("session from cookie failed\n");
		return 1;
	}

	return 0;
}

struct lla {
	char *username;
	int len;
	int results;
};

static int
lwsgs_lookup_callback(void *priv, int cols, char **col_val, char **col_name)
{
	struct lla *lla = (struct lla *)priv;

	//lwsl_err("%s: %d\n", __func__, cols);

	if (cols)
		lla->results = 0;
	if (col_val && col_val[0]) {
		strncpy(lla->username, col_val[0], lla->len);
		lla->username[lla->len - 1] = '\0';
		lwsl_info("%s: %s\n", __func__, lla->username);
	}

	return 0;
}

static int
lwsgs_lookup_session(struct per_vhost_data__generic_sessions *vhd,
		     const lwsgw_hash *sid, char *username, int len)
{
	struct lla lla = { username, len, 1 };
	char s[150], esc[50];

	lwsgw_expire_old_sessions(vhd);

	snprintf(s, sizeof(s) - 1,
		 "select username from sessions where name = '%s';",
		 lws_sql_purify(esc, sid->id, sizeof(esc) - 1));

	if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback, &lla, NULL) != SQLITE_OK) {
		lwsl_err("Unable to create user table: %s\n",
			 sqlite3_errmsg(vhd->pdb));

		return 1;
	}

	/* 0 if found */
	return lla.results;
}

static int
lwsgs_lookup_callback_user(void *priv, int cols, char **col_val, char **col_name)
{
	struct lwsgs_user *u = (struct lwsgs_user *)priv;
	int n;

	for (n = 0; n < cols; n++) {
		if (!strcmp(col_name[n], "username")) {
			strncpy(u->username, col_val[n], sizeof(u->username) - 1);
			u->username[sizeof(u->username) - 1] = '\0';
			continue;
		}
		if (!strcmp(col_name[n], "ip")) {
			strncpy(u->ip, col_val[n], sizeof(u->ip) - 1);
			u->ip[sizeof(u->ip) - 1] = '\0';
			continue;
		}
		if (!strcmp(col_name[n], "creation_time")) {
			u->created = atol(col_val[n]);
			continue;
		}
		if (!strcmp(col_name[n], "last_forgot_validated")) {
			if (col_val[n])
				u->last_forgot_validated = atol(col_val[n]);
			else
				u->last_forgot_validated = 0;
			continue;
		}
		if (!strcmp(col_name[n], "email")) {
			strncpy(u->email, col_val[n], sizeof(u->email) - 1);
			u->email[sizeof(u->email) - 1] = '\0';
			continue;
		}
		if (!strcmp(col_name[n], "verified")) {
			u->verified = atoi(col_val[n]);
			continue;
		}
		if (!strcmp(col_name[n], "pwhash")) {
			strncpy(u->pwhash.id, col_val[n], sizeof(u->pwhash.id) - 1);
			u->pwhash.id[sizeof(u->pwhash.id) - 1] = '\0';
			continue;
		}
		if (!strcmp(col_name[n], "pwsalt")) {
			strncpy(u->pwsalt.id, col_val[n], sizeof(u->pwsalt.id) - 1);
			u->pwsalt.id[sizeof(u->pwsalt.id) - 1] = '\0';
			continue;
		}
		if (!strcmp(col_name[n], "token")) {
			strncpy(u->token.id, col_val[n], sizeof(u->token.id) - 1);
			u->token.id[sizeof(u->token.id) - 1] = '\0';
			continue;
		}
	}
	return 0;
}

static int
lwsgs_lookup_user(struct per_vhost_data__generic_sessions *vhd,
		  const char *username, struct lwsgs_user *u)
{
	char s[150], esc[50];

	u->username[0] = '\0';
	snprintf(s, sizeof(s) - 1,
		 "select username,creation_time,ip,email,verified,pwhash,pwsalt,last_forgot_validated "
		 "from users where username = '%s';",
		 lws_sql_purify(esc, username, sizeof(esc) - 1));

	if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, u, NULL) !=
	    SQLITE_OK) {
		lwsl_err("Unable to lookup user: %s\n",
			 sqlite3_errmsg(vhd->pdb));

		return -1;
	}

	return !u->username[0];
}

static int
lwsgs_new_session_id(struct per_vhost_data__generic_sessions *vhd,
		     lwsgw_hash *sid, const char *username, int exp)
{
	unsigned char sid_rand[20];
	const char *u;
	char s[300], esc[50], esc1[50];

	if (username)
		u = username;
	else
		u = "";

	if (!sid)
		return 1;

	memset(sid, 0, sizeof(*sid));

	if (lws_get_random(vhd->context, sid_rand, sizeof(sid_rand)) !=
			   sizeof(sid_rand))
		return 1;

	sha1_to_lwsgw_hash(sid_rand, sid);

	snprintf(s, sizeof(s) - 1,
		 "insert into sessions(name, username, expire) "
		 "values ('%s', '%s', %u);",
		 lws_sql_purify(esc, sid->id, sizeof(esc) - 1),
		 lws_sql_purify(esc1, u, sizeof(esc1) - 1), exp);

	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("Unable to insert session: %s\n",
			 sqlite3_errmsg(vhd->pdb));

		return 1;
	}

	return 0;
}

static int
lwsgs_get_auth_level(struct per_vhost_data__generic_sessions *vhd,
		     const char *username)
{
	struct lwsgs_user u;
	int n = 0;

	/* we are logged in as some kind of user */
	if (username[0]) {
		n |= LWSGS_AUTH_LOGGED_IN;
		/* we are logged in as admin */
		if (!strcmp(username, vhd->admin_user))
			n |= LWSGS_AUTH_VERIFIED | LWSGS_AUTH_ADMIN; /* automatically verified */
	}

	if (!lwsgs_lookup_user(vhd, username, &u)) {
		if ((u.verified & 0xff) == LWSGS_VERIFIED_ACCEPTED)
			n |= LWSGS_AUTH_VERIFIED;

		if (u.last_forgot_validated > lwsgs_now_secs() - 300)
			n |= LWSGS_AUTH_FORGOT_FLOW;
	}

	return n;
}

struct lwsgs_fill_args {
	char *buf;
	int len;
};

static int
lwsgs_lookup_callback_email(void *priv, int cols, char **col_val, char **col_name)
{
	struct lwsgs_fill_args *a = (struct lwsgs_fill_args *)priv;
	int n;

	for (n = 0; n < cols; n++) {
		if (!strcmp(col_name[n], "content")) {
			strncpy(a->buf, col_val[n], a->len - 1);
			a->buf[a->len - 1] = '\0';
			continue;
		}
	}
	return 0;
}

static int
lwsgs_email_cb_get_body(struct lws_email *email, char *buf, int len)
{
	struct per_vhost_data__generic_sessions *vhd =
		(struct per_vhost_data__generic_sessions *)email->data;
	struct lwsgs_fill_args a;
	char ss[150], esc[50];

	a.buf = buf;
	a.len = len;

	snprintf(ss, sizeof(ss) - 1,
		 "select content from email where username='%s';",
		 lws_sql_purify(esc, vhd->u.username, sizeof(esc) - 1));

	strncpy(buf, "failed", len);
	if (sqlite3_exec(vhd->pdb, ss, lwsgs_lookup_callback_email, &a,
			 NULL) != SQLITE_OK) {
		lwsl_err("Unable to lookup email: %s\n",
			 sqlite3_errmsg(vhd->pdb));

		return 1;
	}

	return 0;
}

static int
lwsgs_email_cb_sent(struct lws_email *email)
{
	struct per_vhost_data__generic_sessions *vhd =
		(struct per_vhost_data__generic_sessions *)email->data;
	char s[200], esc[50];

	/* mark the user as having sent the verification email */
	snprintf(s, sizeof(s) - 1,
		 "update users set verified=1 where username='%s' and verified==0;",
		 lws_sql_purify(esc, vhd->u.username, sizeof(esc) - 1));
	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("%s: Unable to update user: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb));
		return 1;
	}
	snprintf(s, sizeof(s) - 1,
		 "delete from email where username='%s';",
		 lws_sql_purify(esc, vhd->u.username, sizeof(esc) - 1));
	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("%s: Unable to delete email text: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb));
		return 1;
	}

	return 0;
}

static int
lwsgs_email_cb_on_next(struct lws_email *email)
{
	struct per_vhost_data__generic_sessions *vhd = lws_container_of(email,
			struct per_vhost_data__generic_sessions, email);
	char s[LWSGS_EMAIL_CONTENT_SIZE];
	time_t now = lwsgs_now_secs();

	/*
	 * users not verified in 24h get deleted
	 */
	snprintf(s, sizeof(s) - 1,
		 "delete from users where ((verified != %d) and "
		 "(creation_time <= %lu));", LWSGS_VERIFIED_ACCEPTED,
		 (unsigned long)now - vhd->timeout_email_secs);

	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("Unable to expire users: %s\n",
			 sqlite3_errmsg(vhd->pdb));
		return 1;
	}

	snprintf(s, sizeof(s) - 1,
		 "update users set token_time=0 where "
		 "(token_time <= %lu);",
		 (unsigned long)now - vhd->timeout_email_secs);

	if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("Unable to expire users: %s\n",
			 sqlite3_errmsg(vhd->pdb));
		return 1;
	}

	vhd->u.username[0] = '\0';
	snprintf(s, sizeof(s) - 1,
		 "select username from email limit 1;");

	if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &vhd->u,
			 NULL) != SQLITE_OK) {
		lwsl_err("Unable to lookup user: %s\n",
			 sqlite3_errmsg(vhd->pdb));

		return 1;
	}
	snprintf(s, sizeof(s) - 1,
		 "select username, creation_time, email, ip, verified, token"
		 " from users where username='%s' limit 1;",
		 vhd->u.username);

	if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &vhd->u,
			 NULL) != SQLITE_OK) {
		lwsl_err("Unable to lookup user: %s\n",
			 sqlite3_errmsg(vhd->pdb));

		return 1;
	}

	if (!vhd->u.username[0])
		/*
		 * nothing to do, we are idle and no suitable
		 * accounts waiting for verification.  When a new user
		 * is added we will get kicked to try again.
		 */
		return 1;

	strncpy(email->email_to, vhd->u.email, sizeof(email->email_to) - 1);

	return 0;
}


static int
lwsgs_check_credentials(struct per_vhost_data__generic_sessions *vhd,
			const char *username, const char *password)
{
	unsigned char buffer[300];
	lwsgw_hash_bin hash_bin;
	struct lwsgs_user u;
	lwsgw_hash hash;
	int n;

	if (lwsgs_lookup_user(vhd, username, &u))
		return -1;

	lwsl_info("user %s found, salt '%s'\n", username, u.pwsalt.id);

	/* [password in ascii][salt] */
	n = snprintf((char *)buffer, sizeof(buffer) - 1,
		     "%s-%s-%s", password, vhd->confounder, u.pwsalt.id);

	/* sha1sum of password + salt */
	lws_SHA1(buffer, n, hash_bin.bin);
	sha1_to_lwsgw_hash(&hash_bin.bin[0], &hash);

	return !!strcmp(hash.id, u.pwhash.id);
}

/* sets u->pwsalt and u->pwhash */

static int
lwsgs_hash_password(struct per_vhost_data__generic_sessions *vhd,
		    const char *password, struct lwsgs_user *u)
{
	lwsgw_hash_bin hash_bin;
	lwsgw_hash hash;
	unsigned char sid_rand[20];
	unsigned char buffer[150];
	int n;

	/* create a random salt as big as the hash */

	if (lws_get_random(vhd->context, sid_rand,
			   sizeof(sid_rand)) !=
			   sizeof(sid_rand)) {
		lwsl_err("Problem getting random for salt\n");
		return 1;
	}
	sha1_to_lwsgw_hash(sid_rand, &u->pwsalt);

	if (lws_get_random(vhd->context, sid_rand,
			   sizeof(sid_rand)) !=
			   sizeof(sid_rand)) {
		lwsl_err("Problem getting random for token\n");
		return 1;
	}
	sha1_to_lwsgw_hash(sid_rand, &hash);

	/* [password in ascii][salt] */
	n = snprintf((char *)buffer, sizeof(buffer) - 1,
		    "%s-%s-%s", password, vhd->confounder, u->pwsalt.id);

	/* sha1sum of password + salt */
	lws_SHA1(buffer, n, hash_bin.bin);
	sha1_to_lwsgw_hash(&hash_bin.bin[0], &u->pwhash);

	return 0;
}

static int
callback_generic_sessions(struct lws *wsi, enum lws_callback_reasons reason,
			  void *user, void *in, size_t len)
{
	struct per_session_data__generic_sessions *pss =
			(struct per_session_data__generic_sessions *)user;
	const struct lws_protocol_vhost_options *pvo;
	struct per_vhost_data__generic_sessions *vhd =
			(struct per_vhost_data__generic_sessions *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
					lws_get_protocol(wsi));

	const char *cp;
	unsigned char buffer[LWS_PRE + LWSGS_EMAIL_CONTENT_SIZE];
	char cookie[1024], username[32], *pc = cookie, *sp;
	char esc[50], esc1[50], esc2[50], esc3[50], esc4[50];
	struct lws_process_html_args *args;
	unsigned char *p, *start, *end;
	sqlite3_stmt *sm;
	lwsgw_hash sid;
	int n, old_len;
	struct lwsgs_user u;
	char s[LWSGS_EMAIL_CONTENT_SIZE];

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT: /* per vhost */
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
						  lws_get_protocol(wsi),
				sizeof(struct per_vhost_data__generic_sessions));
		vhd->context = lws_get_context(wsi);

		/* defaults */

		vhd->timeout_idle_secs = 600;
		vhd->timeout_absolute_secs = 36000;
		vhd->timeout_anon_absolute_secs = 1200;
		vhd->timeout_email_secs = 24 * 3600;
		strcpy(vhd->email.email_helo, "unconfigured.com");
		strcpy(vhd->email.email_from, "noreply@unconfigured.com");
		strcpy(vhd->email_title, "Registration Email from unconfigured");
		strcpy(vhd->email.email_smtp_ip, "127.0.0.1");

		vhd->email.on_next = lwsgs_email_cb_on_next;
		vhd->email.on_get_body = lwsgs_email_cb_get_body;
		vhd->email.on_sent = lwsgs_email_cb_sent;
		vhd->email.data = (void *)vhd;

		pvo = (const struct lws_protocol_vhost_options *)in;
		while (pvo) {
			if (!strcmp(pvo->name, "admin-user"))
				strncpy(vhd->admin_user, pvo->value,
					sizeof(vhd->admin_user) - 1);
			if (!strcmp(pvo->name, "admin-password-sha1"))
				strncpy(vhd->admin_password_sha1.id, pvo->value,
					sizeof(vhd->admin_password_sha1.id) - 1);
			if (!strcmp(pvo->name, "session-db"))
				strncpy(vhd->session_db, pvo->value,
					sizeof(vhd->session_db) - 1);
			if (!strcmp(pvo->name, "confounder"))
				strncpy(vhd->confounder, pvo->value,
					sizeof(vhd->confounder) - 1);
			if (!strcmp(pvo->name, "email-from"))
				strncpy(vhd->email.email_from, pvo->value,
					sizeof(vhd->email.email_from) - 1);
			if (!strcmp(pvo->name, "email-helo"))
				strncpy(vhd->email.email_helo, pvo->value,
					sizeof(vhd->email.email_helo) - 1);
			if (!strcmp(pvo->name, "email-template"))
				strncpy(vhd->email_template, pvo->value,
					sizeof(vhd->email_template) - 1);
			if (!strcmp(pvo->name, "email-title"))
				strncpy(vhd->email_title, pvo->value,
					sizeof(vhd->email_title) - 1);
			if (!strcmp(pvo->name, "email-contact-person"))
				strncpy(vhd->email_contact_person, pvo->value,
					sizeof(vhd->email_contact_person) - 1);
			if (!strcmp(pvo->name, "email-confirm-url-base"))
				strncpy(vhd->email_confirm_url, pvo->value,
					sizeof(vhd->email_confirm_url) - 1);
			if (!strcmp(pvo->name, "email-server-ip"))
				strncpy(vhd->email.email_smtp_ip, pvo->value,
					sizeof(vhd->email.email_smtp_ip) - 1);

			if (!strcmp(pvo->name, "timeout-idle-secs"))
				vhd->timeout_idle_secs = atoi(pvo->value);
			if (!strcmp(pvo->name, "timeout-absolute-secs"))
				vhd->timeout_absolute_secs = atoi(pvo->value);
			if (!strcmp(pvo->name, "timeout-anon-absolute-secs"))
				vhd->timeout_anon_absolute_secs = atoi(pvo->value);
			if (!strcmp(pvo->name, "email-expire"))
				vhd->timeout_email_secs = atoi(pvo->value);
			pvo = pvo->next;
		}
		if (!vhd->admin_user[0] ||
		    !vhd->admin_password_sha1.id[0] ||
		    !vhd->session_db[0]) {
			lwsl_err("generic-sessions: "
				 "You must give \"admin-user\", "
				 "\"admin-password-sha1\", "
				 "and \"session_db\" per-vhost options\n");
			return 1;
		}

		if (sqlite3_open_v2(vhd->session_db, &vhd->pdb,
				    SQLITE_OPEN_READWRITE |
				    SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
			lwsl_err("Unable to open session db %s: %s\n",
				 vhd->session_db, sqlite3_errmsg(vhd->pdb));

			return 1;
		}

		if (sqlite3_prepare(vhd->pdb,
				    "create table if not exists sessions ("
				    " name char(40),"
				    " username varchar(32),"
				    " expire integer"
				    ");",
				    -1, &sm, NULL) != SQLITE_OK) {
			lwsl_err("Unable to prepare session table init: %s\n",
				 sqlite3_errmsg(vhd->pdb));

			return 1;
		}

		if (sqlite3_step(sm) != SQLITE_DONE) {
			lwsl_err("Unable to run session table init: %s\n",
				 sqlite3_errmsg(vhd->pdb));

			return 1;
		}
		sqlite3_finalize(sm);

		if (sqlite3_exec(vhd->pdb,
				 "create table if not exists users ("
				 " username varchar(32),"
				 " creation_time integer,"
				 " ip varchar(46),"
				 " email varchar(100),"
				 " pwhash varchar(42),"
				 " pwsalt varchar(42),"
				 " pwchange_time integer,"
				 " token varchar(42),"
				 " verified integer,"
				 " token_time integer,"
				 " last_forgot_validated integer,"
				 " primary key (username)"
				 ");",
				 NULL, NULL, NULL) != SQLITE_OK) {
			lwsl_err("Unable to create user table: %s\n",
				 sqlite3_errmsg(vhd->pdb));

			return 1;
		}

		sprintf(s, "create table if not exists email ("
				 " username varchar(32),"
				 " content blob,"
				 " primary key (username)"
				 ");");
		if (sqlite3_exec(vhd->pdb, s,
				 NULL, NULL, NULL) != SQLITE_OK) {
			lwsl_err("Unable to create user table: %s\n",
				 sqlite3_errmsg(vhd->pdb));

			return 1;
		}

		lws_email_init(&vhd->email, lws_uv_getloop(vhd->context, 0),
				LWSGS_EMAIL_CONTENT_SIZE);
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		if (vhd->pdb) {
			sqlite3_close(vhd->pdb);
			vhd->pdb = NULL;
		}
		lws_email_destroy(&vhd->email);
		break;

	case LWS_CALLBACK_HTTP:
		lwsl_notice("LWS_CALLBACK_HTTP: %s\n", in);

		pss->login_session.id[0] = '\0';
		pss->pos = 0;
		strncpy(pss->onward, (char *)in, sizeof(pss->onward));

		if (!strcmp((const char *)in, "/forgot")) {
			const char *a;

			a = lws_get_urlarg_by_name(wsi, "token=", cookie,
						     sizeof(cookie));
			if (!a)
				goto forgot_fail;

			u.username[0] = '\0';
			snprintf(s, sizeof(s) - 1,
				 "select username,verified "
				 "from users where verified=%d and "
				 "token = '%s' and token_time != 0;",
				 LWSGS_VERIFIED_ACCEPTED,
				 lws_sql_purify(esc, &cookie[6], sizeof(esc) - 1));
			if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &u, NULL) !=
			    SQLITE_OK) {
				lwsl_err("Unable to lookup token: %s\n",
					 sqlite3_errmsg(vhd->pdb));

				goto forgot_fail;
			}

			if (!u.username[0]) {
				puts(s);
				lwsl_notice("forgot token doesn't map to verified user\n");
				goto forgot_fail;
			}

			/* mark user as having validated forgot flow just now */

			snprintf(s, sizeof(s) - 1,
				 "update users set token_time=0,last_forgot_validated=%lu  where username='%s';",
				 (unsigned long)lwsgs_now_secs(),
				 lws_sql_purify(esc, u.username, sizeof(esc) - 1));

			if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &u, NULL) !=
			    SQLITE_OK) {
				lwsl_err("Unable to lookup token: %s\n",
					 sqlite3_errmsg(vhd->pdb));

				goto forgot_fail;
			}

			a = lws_get_urlarg_by_name(wsi, "good=", cookie,
						     sizeof(cookie));
			if (!a)
				a = "broken-forget-post-good-url";

			snprintf(pss->onward, sizeof(pss->onward),
				 "%s/%s", vhd->email_confirm_url, a);

			pss->login_expires = lwsgs_now_secs() +
					     vhd->timeout_absolute_secs;

			pss->delete_session.id[0] = '\0';
			lwsgs_get_sid_from_wsi(wsi, &pss->delete_session);

			/* we need to create a new, authorized session */
			if (lwsgs_new_session_id(vhd, &pss->login_session,
						 u.username,
						 pss->login_expires))
				goto forgot_fail;

			lwsl_notice("Creating new session: %s, redir to %s\n",
				    pss->login_session.id, pss->onward);

			goto redirect_with_cookie;

forgot_fail:
			pss->delete_session.id[0] = '\0';
			lwsgs_get_sid_from_wsi(wsi, &pss->delete_session);
			pss->login_expires = 0;

			a = lws_get_urlarg_by_name(wsi, "bad=", cookie,
						     sizeof(cookie));
			if (!a)
				a = "broken-forget-post-bad-url";

			snprintf(pss->onward, sizeof(pss->onward),
				 "%s/%s", vhd->email_confirm_url, a);

			goto redirect_with_cookie;
		}

		if (!strcmp((const char *)in, "/confirm")) {

			if (lws_hdr_copy_fragment(wsi, cookie, sizeof(cookie),
						  WSI_TOKEN_HTTP_URI_ARGS, 0) < 0) {
				lwsl_notice("copy failed\n");
				goto verf_fail;
			}

			if (strncmp(cookie, "token=", 6)) {
				lwsl_notice("not token=\n");
				goto verf_fail;
			}

			u.username[0] = '\0';
			snprintf(s, sizeof(s) - 1,
				 "select username,verified "
				 "from users where token = '%s';",
				 lws_sql_purify(esc, &cookie[6], sizeof(esc) - 1));
			if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &u, NULL) !=
			    SQLITE_OK) {
				lwsl_err("Unable to lookup token: %s\n",
					 sqlite3_errmsg(vhd->pdb));

				goto verf_fail;
			}

			if (!u.username[0] || u.verified != 1) {
				lwsl_notice("verify token doesn't map to unverified user\n");
				goto verf_fail;
			}

			lwsl_notice("Verifying %s\n", u.username);
			snprintf(s, sizeof(s) - 1,
				 "update users set verified=%d where username='%s';",
				 LWSGS_VERIFIED_ACCEPTED,
				 lws_sql_purify(esc, u.username, sizeof(esc) - 1));
			if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &u, NULL) !=
			    SQLITE_OK) {
				lwsl_err("Unable to lookup token: %s\n",
					 sqlite3_errmsg(vhd->pdb));

				goto verf_fail;
			}

			snprintf(pss->onward, sizeof(pss->onward),
				 "%s/post-verify-ok.html",
				 vhd->email_confirm_url);

			pss->login_expires = lwsgs_now_secs() +
					     vhd->timeout_absolute_secs;

			pss->delete_session.id[0] = '\0';
			lwsgs_get_sid_from_wsi(wsi, &pss->delete_session);

			/* we need to create a new, authorized session */

			if (lwsgs_new_session_id(vhd, &pss->login_session,
						 u.username,
						 pss->login_expires))
				goto verf_fail;

			lwsl_notice("Creating new session: %s, redir to %s\n",
				    pss->login_session.id, pss->onward);

			goto redirect_with_cookie;

verf_fail:
			pss->delete_session.id[0] = '\0';
			lwsgs_get_sid_from_wsi(wsi, &pss->delete_session);
			pss->login_expires = 0;

			snprintf(pss->onward, sizeof(pss->onward),
				 "%s/post-verify-fail.html",
				  vhd->email_confirm_url);

			goto redirect_with_cookie;
		}
		if (!strcmp((const char *)in, "/check")) {
			/*
			 * either /check?email=xxx@yyy
			 *
			 * or, /check?username=xxx
			 *
			 * returns '0' if not already registered, else '1'
			 */

			static const char * const colname[] = {
				"username", "email"
			};

			u.username[0] = '\0';
			if (lws_hdr_copy_fragment(wsi, cookie, sizeof(cookie),
						  WSI_TOKEN_HTTP_URI_ARGS, 0) < 0)
				goto nope;

			n = !strncmp(cookie, "email=", 6);
			pc = strchr(cookie, '=');
			if (!pc) {
				lwsl_notice("cookie has no =\n");
				goto nope;
			}
			pc++;

			snprintf(s, sizeof(s) - 1,
				 "select username, email "
				 "from users where %s = '%s';",
				 colname[n],
				 lws_sql_purify(esc, pc, sizeof(esc) - 1));

			if (sqlite3_exec(vhd->pdb, s,
					 lwsgs_lookup_callback_user, &u, NULL) !=
			    SQLITE_OK) {
				lwsl_err("Unable to lookup token: %s\n",
					 sqlite3_errmsg(vhd->pdb));
				goto nope;
			}
nope:
			s[0] = '0' + !!u.username[0];
			p = buffer + LWS_PRE;
			start = p;
			end = p + sizeof(buffer) - LWS_PRE;

			if (lws_add_http_header_status(wsi, 200, &p, end))
				return -1;
			if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
							 (unsigned char *)"text/plain", 10,
							 &p, end))
				return -1;

			if (lws_add_http_header_content_length(wsi, 1, &p, end))
					return -1;

			if (lws_finalize_http_header(wsi, &p, end))
				return -1;

			n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
			if (n != (p - start)) {
				lwsl_err("_write returned %d from %d\n",
					 n, (p - start));
				return -1;
			}
			n = lws_write(wsi, (unsigned char *)s, 1, LWS_WRITE_HTTP);
			if (n != 1)
				return -1;
			goto try_to_reuse;
		}

		if (!strcmp((const char *)in, "/login"))
			break;
		if (!strcmp((const char *)in, "/logout"))
			break;
		if (!strcmp((const char *)in, "/forgot"))
			break;
		if (!strcmp((const char *)in, "/change"))
			break;

		lwsl_err("http doing 404 on %s\n", in);
		lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
		break;

	case LWS_CALLBACK_CHECK_ACCESS_RIGHTS:
		n = 0;
		username[0] = '\0';
		sid.id[0] = '\0';
		args = (struct lws_process_html_args *)in;
		lwsl_debug("LWS_CALLBACK_CHECK_ACCESS_RIGHTS\n");
		if (!lwsgs_get_sid_from_wsi(wsi, &sid)) {
			if (lwsgs_lookup_session(vhd, &sid, username, sizeof(username))) {
				static const char * const oprot[] = {
					"http://", "https://"
				};
				lwsl_notice("session lookup for %s failed, probably expired\n", sid.id);
				pss->delete_session = sid;
				args->final = 1; /* signal we dealt with it */
				if (lws_hdr_copy(wsi, cookie, sizeof(cookie) - 1,
					     WSI_TOKEN_HOST) < 0)
					return 1;
				snprintf(pss->onward, sizeof(pss->onward) - 1,
					 "%s%s%s", oprot[lws_is_ssl(wsi)],
					    cookie, args->p);
				lwsl_notice("redirecting to ourselves with cookie refresh\n");
				/* we need a redirect to ourselves, session cookie is expired */
				goto redirect_with_cookie;
			}
		} else
			lwsl_notice("failed to get sid from wsi\n");

		n = lwsgs_get_auth_level(vhd, username);

		if ((args->max_len & n) != args->max_len) {
			lwsl_notice("Access rights fail 0x%X vs 0x%X (cookie %s)\n",
					args->max_len, n, sid.id);
			return 1;
		}
		lwsl_debug("Access rights OK\n");
		break;

	case LWS_CALLBACK_PROCESS_HTML:
		/*
		 * replace placeholders with session data and prepare the
		 * preamble to send chunked, p is already at +10 from the
		 * real buffer start to allow us to add the chunk header
		 *
		 * struct lws_process_html_args {
		 *	char *p;
		 *	int len;
		 *	int max_len;
		 *	int final;
		 * };
		 */

		args = (struct lws_process_html_args *)in;

		username[0] = '\0';
		u.email[0] = '\0';
		if (!lwsgs_get_sid_from_wsi(wsi, &sid)) {
			if (lwsgs_lookup_session(vhd, &sid, username,
						 sizeof(username))) {
				lwsl_notice("sid lookup for %s failed\n", sid.id);
				pss->delete_session = sid;
				return 1;
			}
			snprintf(s, sizeof(s) - 1,
				 "select username,email "
				 "from users where username = '%s';",
				 lws_sql_purify(esc, username, sizeof(esc) - 1));

			if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user,
					 &u, NULL) != SQLITE_OK) {
				lwsl_err("Unable to lookup token: %s\n",
					 sqlite3_errmsg(vhd->pdb));
				pss->delete_session = sid;
				return 1;
			}
		} else
			lwsl_notice("no sid\n");

		/* do replacements */
		sp = args->p;
		old_len = args->len;
		args->len = 0;
		pss->start = sp;
		while (sp < args->p + old_len) {

			if (args->len + 7 >= args->max_len) {
				lwsl_err("Used up interpret padding\n");
				return -1;
			}

			if ((!pss->pos && *sp == '$') ||
			    pss->pos) {
				static const char * const vars[] = {
					"$lwsgs_user",
					"$lwsgs_auth",
					"$lwsgs_email"
				};
				int hits = 0, hit;

				if (!pss->pos)
					pss->start = sp;
				pss->swallow[pss->pos++] = *sp;
				if (pss->pos == sizeof(pss->swallow))
					goto skip;
				for (n = 0; n < ARRAY_SIZE(vars); n++)
					if (!strncmp(pss->swallow, vars[n], pss->pos)) {
						hits++;
						hit = n;
					}
				if (!hits) {
skip:
					pss->swallow[pss->pos] = '\0';
					memcpy(pss->start, pss->swallow, pss->pos);
					args->len++;
					pss->pos = 0;
					sp = pss->start + 1;
					continue;
				}
				if (hits == 1 && pss->pos == strlen(vars[hit])) {
					switch (hit) {
					case 0:
						pc = username;
						break;
					case 1:
						pc = cookie;
						n = lwsgs_get_auth_level(vhd, username);
						sprintf(cookie, "%d", n);
						break;
					case 2:
						pc = u.email;
						break;
					}

					n = strlen(pc);
					pss->swallow[pss->pos] = '\0';
					if (n != pss->pos) {
						memmove(pss->start + n,
							pss->start + pss->pos,
							old_len -
							   (sp - args->p));
						old_len += (n - pss->pos) + 1;
					}
					memcpy(pss->start, pc, n);
					args->len++;
					sp = pss->start + 1;

					pss->pos = 0;
				}
				sp++;
				continue;
			}

			args->len++;
			sp++;
		}

		/* no space left for final chunk trailer */
		if (args->final && args->len + 7 >= args->max_len)
			return -1;

		n = sprintf((char *)buffer, "%X\x0d\x0a", args->len);

		args->p -= n;
		memcpy(args->p, buffer, n);
		args->len += n;

		if (args->final) {
			sp = args->p + args->len;
			*sp++ = '\x0d';
			*sp++ = '\x0a';
			*sp++ = '0';
			*sp++ = '\x0d';
			*sp++ = '\x0a';
			*sp++ = '\x0d';
			*sp++ = '\x0a';
			args->len += 7;
		} else {
			sp = args->p + args->len;
			*sp++ = '\x0d';
			*sp++ = '\x0a';
			args->len += 2;
		}
		break;

	case LWS_CALLBACK_HTTP_BODY:
		lwsl_notice("LWS_CALLBACK_HTTP_BODY: %s %d\n", pss->onward, len);

		if (len < 2)
			break;

		if (!pss->spa) {
			pss->spa = lws_urldecode_spa_create(param_names,
						ARRAY_SIZE(param_names), 1024,
						NULL, NULL);
			if (!pss->spa)
				return -1;
		}

		if (lws_urldecode_spa_process(pss->spa, in, len)) {
			lwsl_notice("spa process blew\n");
			return -1;
		}

		break;

	case LWS_CALLBACK_HTTP_WRITEABLE:
		break;

	case LWS_CALLBACK_HTTP_BODY_COMPLETION:

		if (!pss->spa)
			break;

		lwsl_notice("LWS_CALLBACK_HTTP_BODY_COMPLETION: %s\n", pss->onward);

		lws_urldecode_spa_finalize(pss->spa);

		/*
		 * change password
		 */

		if (!strcmp((char *)pss->onward, "/change")) {
			n = 0;

			/* see if he's logged in */
			username[0] = '\0';
			if (!lwsgs_get_sid_from_wsi(wsi, &sid)) {
				u.username[0] = '\0';
				if (!lwsgs_lookup_session(vhd, &sid, username, sizeof(username))) {
					n = 1; /* yes, logged in */
					if (lwsgs_lookup_user(vhd, username, &u))
						goto change_fail;

					/* did a forgot pw ? */
					if (u.last_forgot_validated >
					    lwsgs_now_secs() - 300)
						n |= LWSGS_AUTH_FORGOT_FLOW;
				}
			}

			/* if he just did forgot pw flow, don't need old pw */
			if (!(n & (LWSGS_AUTH_FORGOT_FLOW | 1))) {
				/* otherwise user:pass must be right */
				if (lwsgs_check_credentials(vhd,
						lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
						lws_urldecode_spa_get_string(pss->spa, FGS_CURPW))) {
					lwsl_notice("credentials bad\n");
					goto change_fail;
				}

				strcpy(u.username, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME));
			}

			if (lwsgs_hash_password(vhd, lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD),
					        &u)) {
				lwsl_err("Password hash failed\n");
				goto change_fail;
			}

			lwsl_notice("updating password hash\n");

			snprintf(s, sizeof(s) - 1,
				 "update users set pwhash='%s', pwsalt='%s', "
				 "last_forgot_validated=0 where username='%s';",
				 u.pwhash.id, u.pwsalt.id,
				 lws_sql_purify(esc, u.username, sizeof(esc) - 1));
			if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) !=
			    SQLITE_OK) {
				lwsl_err("Unable to update pw hash: %s\n",
					 sqlite3_errmsg(vhd->pdb));

				goto change_fail;
			}

			cp = lws_urldecode_spa_get_string(pss->spa, FGS_GOOD);
			goto pass;
change_fail:
			cp = lws_urldecode_spa_get_string(pss->spa, FGS_BAD);
			lwsl_notice("user/password no good %s\n",
					lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME));
			strncpy(pss->onward, cp, sizeof(pss->onward) - 1);
			pss->onward[sizeof(pss->onward) - 1] = '\0';
			goto completion_flow;
		}

		if (!strcmp((char *)pss->onward, "/login")) {
			lwsgw_hash hash;
			unsigned char sid_rand[20];

			if (lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT) &&
			    lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT)[0]) {

				lwsl_notice("FORGOT %s %s\n",
					    lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
					    lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL));

				if (!lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME) &&
				    !lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL)) {
					lwsl_err("Form must provide either "
						  "username or email\n");
					return -1;
				}

				if (!lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT_GOOD) ||
				    !lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT_BAD) ||
				    !lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT_POST_GOOD) ||
				    !lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT_POST_BAD)) {
					lwsl_err("Form must provide reg-good "
						  "and reg-bad (and post-*)"
						  "targets\n");
					return -1;
				}

				u.username[0] = '\0';
				if (lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME))
					snprintf(s, sizeof(s) - 1,
					 "select username,email "
					 "from users where username = '%s';",
					 lws_sql_purify(esc, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
							 sizeof(esc) - 1));
				else
					snprintf(s, sizeof(s) - 1,
					 "select username,email "
					 "from users where email = '%s';",
					 lws_sql_purify(esc, lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL), sizeof(esc) - 1));
				if (sqlite3_exec(vhd->pdb, s, lwsgs_lookup_callback_user, &u, NULL) !=
				    SQLITE_OK) {
					lwsl_err("Unable to lookup token: %s\n",
						 sqlite3_errmsg(vhd->pdb));

					n = FGS_FORGOT_BAD;
					goto reg_done;
				}
				if (!u.username[0]) {
					lwsl_err("No match found %s\n", s);
					n = FGS_FORGOT_BAD;
					goto reg_done;
				}

				lws_get_peer_simple(wsi, pss->ip, sizeof(pss->ip));
				if (lws_get_random(vhd->context, sid_rand,
						   sizeof(sid_rand)) !=
						   sizeof(sid_rand)) {
					lwsl_err("Problem getting random for token\n");
					n = FGS_BAD;
					goto reg_done;
				}
				sha1_to_lwsgw_hash(sid_rand, &hash);
				n = snprintf(s, sizeof(s),
					"From: Forgot Password Assistant Noreply <%s>\n"
					"To: %s <%s>\n"
					  "Subject: Password reset request\n"
					  "\n"
					  "Hello, %s\n\n"
					  "We received a password reset request from IP %s for this email,\n"
					  "to confirm you want to do that, please click the link below.\n\n",
					lws_sql_purify(esc, vhd->email.email_from, sizeof(esc) - 1),
					lws_sql_purify(esc1, u.username, sizeof(esc1) - 1),
					lws_sql_purify(esc2, u.email, sizeof(esc2) - 1),
					lws_sql_purify(esc3, u.username, sizeof(esc3) - 1),
					lws_sql_purify(esc4, pss->ip, sizeof(esc4) - 1));
				snprintf(s + n, sizeof(s) -n,
					  "%s/forgot?token=%s"
					   "&good=%s"
					   "&bad=%s\n\n"
					  "If this request is unexpected, please ignore it and\n"
					  "no further action will be taken.\n\n"
					"If you have any questions or concerns about this\n"
					"automated email, you can contact a real person at\n"
					"%s.\n"
					"\n.\n",
					vhd->email_confirm_url, hash.id,
					lws_urlencode(esc1,
						     lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT_POST_GOOD),
						     sizeof(esc1) - 1),
					lws_urlencode(esc3,
						      lws_urldecode_spa_get_string(pss->spa, FGS_FORGOT_POST_BAD),
						      sizeof(esc3) - 1),
					vhd->email_contact_person);

				snprintf((char *)buffer, sizeof(buffer) - 1,
					 "insert into email(username, content)"
					 " values ('%s', '%s');",
					lws_sql_purify(esc, u.username, sizeof(esc) - 1), s);
				if (sqlite3_exec(vhd->pdb, (char *)buffer, NULL,
						 NULL, NULL) != SQLITE_OK) {
					lwsl_err("Unable to insert email: %s\n",
						 sqlite3_errmsg(vhd->pdb));

					n = FGS_FORGOT_BAD;
					goto reg_done;
				}

				snprintf(s, sizeof(s) - 1,
					 "update users set token='%s',token_time='%ld' where username='%s';",
					 hash.id, (long)lwsgs_now_secs(),
					 lws_sql_purify(esc, u.username, sizeof(esc) - 1));
				if (sqlite3_exec(vhd->pdb, s, NULL, NULL, NULL) !=
				    SQLITE_OK) {
					lwsl_err("Unable to set token: %s\n",
						 sqlite3_errmsg(vhd->pdb));

					n = FGS_FORGOT_BAD;
					goto reg_done;
				}

				/* get the email monitor to take a look */
				lws_email_check(&vhd->email);

				n = FGS_FORGOT_GOOD;
				goto reg_done;
			}

			if (!lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME) ||
			    !lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD)) {
				lwsl_notice("username '%s' or pw '%s' missing\n",
						lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
						lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD));
				return -1;
			}

			if (lws_urldecode_spa_get_string(pss->spa, FGS_REGISTER) &&
			    lws_urldecode_spa_get_string(pss->spa, FGS_REGISTER)[0]) {
				unsigned char sid_rand[20];

				lwsl_notice("REGISTER %s %s %s\n",
						lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
						lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD),
						lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL));
				if (lwsgs_get_sid_from_wsi(wsi,
				    &pss->login_session))
					return 1;

				lws_get_peer_simple(wsi, pss->ip, sizeof(pss->ip));
				lwsl_notice("IP=%s\n", pss->ip);

				if (!lws_urldecode_spa_get_string(pss->spa, FGS_REG_GOOD) ||
				    !lws_urldecode_spa_get_string(pss->spa, FGS_REG_BAD)) {
					lwsl_info("Form must provide reg-good "
						  "and reg-bad targets\n");
					return -1;
				}

				/* admin user cannot be registered in user db */
				if (!strcmp(vhd->admin_user, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME))) {
					n = FGS_REG_BAD;
					goto reg_done;
				}

				if (!lwsgs_lookup_user(vhd,
						lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME), &u)) {
					lwsl_notice("user %s already registered\n",
							lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME));
					n = FGS_REG_BAD;
					goto reg_done;
				}

				u.username[0] = '\0';
				snprintf(s, sizeof(s) - 1,
					 "select username, email "
					 "from users where email = '%s';",
					 lws_sql_purify(esc, lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL),
					 sizeof(esc) - 1));

				if (sqlite3_exec(vhd->pdb, s,
						 lwsgs_lookup_callback_user, &u, NULL) !=
				    SQLITE_OK) {
					lwsl_err("Unable to lookup token: %s\n",
						 sqlite3_errmsg(vhd->pdb));
					n = FGS_REG_BAD;
					goto reg_done;
				}

				if (u.username[0]) {
					lwsl_notice("email %s already in use\n",
						    lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME));
					n = FGS_REG_BAD;
					goto reg_done;
				}

				if (lwsgs_hash_password(vhd,
						        lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD),
						        &u)) {
					lwsl_err("Password hash failed\n");
					n = FGS_REG_BAD;
					goto reg_done;
				}

				if (lws_get_random(vhd->context, sid_rand,
						   sizeof(sid_rand)) !=
						   sizeof(sid_rand)) {
					lwsl_err("Problem getting random for token\n");
					return 1;
				}
				sha1_to_lwsgw_hash(sid_rand, &hash);

				snprintf((char *)buffer, sizeof(buffer) - 1,
					 "insert into users(username,"
					 " creation_time, ip, email, verified,"
					 " pwhash, pwsalt, token, last_forgot_validated)"
					 " values ('%s', %lu, '%s', '%s', 0,"
					 " '%s', '%s', '%s', 0);",
					lws_sql_purify(esc, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME), sizeof(esc) - 1),
					(unsigned long)lwsgs_now_secs(),
					lws_sql_purify(esc1, pss->ip, sizeof(esc1) - 1),
					lws_sql_purify(esc2, lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL), sizeof(esc2) - 1),
					u.pwhash.id, u.pwsalt.id, hash.id);

				n = FGS_REG_GOOD;
				if (sqlite3_exec(vhd->pdb, (char *)buffer, NULL,
						 NULL, NULL) != SQLITE_OK) {
					lwsl_err("Unable to insert user: %s\n",
						 sqlite3_errmsg(vhd->pdb));

					n = FGS_REG_BAD;
					goto reg_done;
				}

				snprintf(s, sizeof(s),
					"From: Noreply <%s>\n"
					"To: %s <%s>\n"
					  "Subject: Registration verification\n"
					  "\n"
					  "Hello, %s\n\n"
					  "We received a registration from IP %s using this email,\n"
					  "to confirm it is legitimate, please click the link below.\n\n"
					  "%s/confirm?token=%s\n\n"
					  "If this request is unexpected, please ignore it and\n"
					  "no further action will be taken.\n\n"
					"If you have any questions or concerns about this\n"
					"automated email, you can contact a real person at\n"
					"%s.\n"
					"\n.\n",
					lws_sql_purify(esc, vhd->email.email_from, sizeof(esc) - 1),
					lws_sql_purify(esc1, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME), sizeof(esc1) - 1),
					lws_sql_purify(esc2, lws_urldecode_spa_get_string(pss->spa, FGS_EMAIL), sizeof(esc2) - 1),
					lws_sql_purify(esc3, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME), sizeof(esc3) - 1),
					lws_sql_purify(esc4, pss->ip, sizeof(esc4) - 1),
					vhd->email_confirm_url, hash.id,
					vhd->email_contact_person);

				snprintf((char *)buffer, sizeof(buffer) - 1,
					 "insert into email(username, content)"
					 " values ('%s', '%s');",
					lws_sql_purify(esc, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME), sizeof(esc) - 1), s);

				if (sqlite3_exec(vhd->pdb, (char *)buffer, NULL,
						 NULL, NULL) != SQLITE_OK) {
					lwsl_err("Unable to insert email: %s\n",
						 sqlite3_errmsg(vhd->pdb));

					n = FGS_REG_BAD;
					goto reg_done;
				}

				/* get the email monitor to take a look */
				lws_email_check(&vhd->email);

reg_done:
				strncpy(pss->onward, lws_urldecode_spa_get_string(pss->spa, n),
					sizeof(pss->onward) - 1);
				pss->onward[sizeof(pss->onward) - 1] = '\0';

				pss->login_expires = 0;
				pss->logging_out = 1;
				goto completion_flow;
			}

			/* we have the username and password... check if admin */
			if (lwsgw_check_admin(vhd, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
					      lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD))) {
				if (lws_urldecode_spa_get_string(pss->spa, FGS_ADMIN))
					cp = lws_urldecode_spa_get_string(pss->spa, FGS_ADMIN);
				else
					if (lws_urldecode_spa_get_string(pss->spa, FGS_GOOD))
						cp = lws_urldecode_spa_get_string(pss->spa, FGS_GOOD);
					else {
						lwsl_info("No admin or good target url in form\n");
						return -1;
					}
				lwsl_debug("admin\n");
				goto pass;
			}

			/* check users in database */

			if (!lwsgs_check_credentials(vhd, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
						     lws_urldecode_spa_get_string(pss->spa, FGS_PASSWORD))) {
				lwsl_info("pw hash check met\n");
				cp = lws_urldecode_spa_get_string(pss->spa, FGS_GOOD);
				goto pass;
			} else
				lwsl_notice("user/password no good %s\n",
						lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME));

			if (!lws_urldecode_spa_get_string(pss->spa, FGS_BAD)) {
				lwsl_info("No admin or good target url in form\n");
				return -1;
			}

			strncpy(pss->onward, lws_urldecode_spa_get_string(pss->spa, FGS_BAD),
				sizeof(pss->onward) - 1);
			pss->onward[sizeof(pss->onward) - 1] = '\0';
			lwsl_debug("failed\n");

			goto completion_flow;
		}

		if (!strcmp((char *)pss->onward, "/logout")) {

			lwsl_notice("/logout\n");

			if (lwsgs_get_sid_from_wsi(wsi, &pss->login_session)) {
				lwsl_notice("not logged in...\n");
				return 1;
			}

			lwsgw_update_session(vhd, &pss->login_session, "");

			if (!lws_urldecode_spa_get_string(pss->spa, FGS_GOOD)) {
				lwsl_info("No admin or good target url in form\n");
				return -1;
			}

			strncpy(pss->onward, lws_urldecode_spa_get_string(pss->spa, FGS_GOOD), sizeof(pss->onward) - 1);
			pss->onward[sizeof(pss->onward) - 1] = '\0';

			pss->login_expires = 0;
			pss->logging_out = 1;

			goto completion_flow;
		}

		break;

pass:

		strncpy(pss->onward, cp, sizeof(pss->onward) - 1);
		pss->onward[sizeof(pss->onward) - 1] = '\0';

		if (lwsgs_get_sid_from_wsi(wsi, &sid))
			sid.id[0] = '\0';

		pss->login_expires = lwsgs_now_secs() +
				     vhd->timeout_absolute_secs;

		if (!sid.id[0]) {
			/* we need to create a new, authorized session */

			if (lwsgs_new_session_id(vhd, &pss->login_session,
						 lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME),
						 pss->login_expires))
				goto try_to_reuse;

			lwsl_notice("Creating new session: %s\n",
				    pss->login_session.id);
		} else {
			/*
			 * we can just update the existing session to be
			 * authorized
			 */
			lwsl_notice("Authorizing current session %s", sid.id);
			lwsgw_update_session(vhd, &sid, lws_urldecode_spa_get_string(pss->spa, FGS_USERNAME));
			pss->login_session = sid;
		}

completion_flow:

		lwsl_notice("LWS_CALLBACK_HTTP_BODY_COMPLETION: onward=%s\n", pss->onward);

		lwsgw_expire_old_sessions(vhd);

redirect_with_cookie:
		p = buffer + LWS_PRE;
		start = p;
		end = p + sizeof(buffer) - LWS_PRE;

		if (lws_add_http_header_status(wsi, HTTP_STATUS_SEE_OTHER, &p, end))
			return 1;

		if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_LOCATION,
				(unsigned char *)pss->onward,
				strlen(pss->onward), &p, end))
			return 1;

		if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
				(unsigned char *)"text/html", 9, &p, end))
			return 1;
		if (lws_add_http_header_content_length(wsi, 0, &p, end))
			return 1;



		if (pss->delete_session.id[0]) {
			lwsgw_cookie_from_session(&pss->delete_session, 0, &pc,
						  cookie + sizeof(cookie) - 1);

			lwsl_notice("deleting cookie '%s'\n", cookie);

			if (lws_add_http_header_by_name(wsi,
					(unsigned char *)"set-cookie:",
					(unsigned char *)cookie, pc - cookie,
					&p, end))
				return 1;
		}

		if (!pss->login_session.id[0]) {
			pss->login_expires = lwsgs_now_secs() +
					     vhd->timeout_anon_absolute_secs;
			if (lwsgs_new_session_id(vhd, &pss->login_session, "",
						 pss->login_expires))
				return 1;
		} else
			pss->login_expires = lwsgs_now_secs() +
					     vhd->timeout_absolute_secs;

		if (pss->login_session.id[0] || pss->logging_out) {
			/*
			 * we succeeded to login, we must issue a login
			 * cookie with the prepared data
			 */
			pc = cookie;

			lwsgw_cookie_from_session(&pss->login_session,
						  pss->login_expires, &pc,
						  cookie + sizeof(cookie) - 1);

			lwsl_notice("setting cookie '%s'\n", cookie);

			pss->logging_out = 0;

			if (lws_add_http_header_by_name(wsi,
					(unsigned char *)"set-cookie:",
					(unsigned char *)cookie, pc - cookie,
					&p, end))
				return 1;
		}

		if (lws_finalize_http_header(wsi, &p, end))
			return 1;

		n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
		if (n < 0)
			return 1;
		goto try_to_reuse;

	case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
		if (pss->spa) {
			lws_urldecode_spa_destroy(pss->spa);
			pss->spa = NULL;
		}
		break;

	case LWS_CALLBACK_ADD_HEADERS:
		lwsgw_expire_old_sessions(vhd);

		args = (struct lws_process_html_args *)in;

		if (pss->delete_session.id[0]) {
			pc = cookie;
			lwsgw_cookie_from_session(&pss->delete_session, 0, &pc,
						  cookie + sizeof(cookie) - 1);

			lwsl_notice("deleting cookie '%s'\n", cookie);

			if (lws_add_http_header_by_name(wsi,
					(unsigned char *)"set-cookie:",
					(unsigned char *)cookie, pc - cookie,
					(unsigned char **)&args->p,
					(unsigned char *)args->p + args->max_len))
				return 1;
		}

		if (!pss->login_session.id[0])
			lwsgs_get_sid_from_wsi(wsi, &pss->login_session);

		if (!pss->login_session.id[0] && !pss->logging_out) {

			pss->login_expires = lwsgs_now_secs() +
					     vhd->timeout_anon_absolute_secs;
			if (lwsgs_new_session_id(vhd, &pss->login_session, "",
						 pss->login_expires))
				goto try_to_reuse;
			pc = cookie;
			lwsgw_cookie_from_session(&pss->login_session,
						  pss->login_expires, &pc,
						  cookie + sizeof(cookie) - 1);

			lwsl_notice("LWS_CALLBACK_ADD_HEADERS: setting cookie '%s'\n", cookie);
			if (lws_add_http_header_by_name(wsi,
					(unsigned char *)"set-cookie:",
					(unsigned char *)cookie, pc - cookie,
					(unsigned char **)&args->p,
					(unsigned char *)args->p + args->max_len))
				return 1;
		}
		break;

	default:
		break;
	}

	return 0;

try_to_reuse:
	if (lws_http_transaction_completed(wsi))
		return -1;

	return 0;
}

static const struct lws_protocols protocols[] = {
	{
		"protocol-generic-sessions",
		callback_generic_sessions,
		sizeof(struct per_session_data__generic_sessions),
		1024,
	},
};

LWS_EXTERN LWS_VISIBLE int
init_protocol_generic_sessions(struct lws_context *context,
			struct lws_plugin_capability *c)
{
	if (c->api_magic != LWS_PLUGIN_API_MAGIC) {
		lwsl_err("Plugin API %d, library API %d", LWS_PLUGIN_API_MAGIC,
			 c->api_magic);
		return 1;
	}

	c->protocols = protocols;
	c->count_protocols = ARRAY_SIZE(protocols);
	c->extensions = NULL;
	c->count_extensions = 0;

	return 0;
}

LWS_EXTERN LWS_VISIBLE int
destroy_protocol_generic_sessions(struct lws_context *context)
{
	return 0;
}
