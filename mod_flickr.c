#include "httpd.h"
#include "http_config.h"
#include "http_connection.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_log.h"

#include "ap_config.h"
#include "apr_hooks.h"
#include "apr_hash.h"
#include "apr_tables.h"
#include "apr_strings.h"
#include "apr_time.h"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/types.h>
#include "md5.h"

#include "flick.h"

/*
 * mod_flickr
 * Apache module curl'ing flickr api's to
 * retrieve, update user's album. Can be used
 * to display a users photos in an album in an
 * iframe (with scrolls etc...)
 * 
 * Implemented as a handler hook, the query
 * request is as follows:
 * http://www.whatsoever.com/<username>/<page>
 */

/*
 * The following are initialized by each
 * child process in the child_init hook.
 *
 * They are _strictly_ supposed to be read
 * only.
 */

typedef struct {
	char *nr_display_photos;	/* string rep. of FLICKR_NR_DISPLAY_PHOTOS		*/
	char *nr_photos_per_call;	/* string rep. of FLICKR_NR_PHOTOS_PER_CALL		*/
	char *user_id;				/* "user_id" string								*/
	char *who;					/* who: me ?									*/

	/*
	 * this hash contains the api
	 * name as the key and a pointer
	 * to the function that implements
	 * the API as the value.
	 */
	apr_hash_t *api_call_table;
} svr_constants;

svr_constants *svr_cfg;

module AP_MODULE_DECLARE_DATA mod_flickr;

/* --------------------------------------------------------- */
/*						HELPER ROUTINES						 */

/*
 * Generate MD5 hash for the string.
 * XXX: Uses request pool to store the
 * generated hash, no need to free().
 */
static char
*flickr_md5_gen(apr_pool_t *p, char *str)
{
        return MD5_string(p, str);
}

/*
 * Preapare the signature string for
 * which the hash has to be calculated.
 */
static char
*flickr_signature_string(apr_pool_t *p, page_data *pg)
{
        return (apr_psprintf(p, FLICKR_SIGNATURE_STRING,
										SECRET(pg),
										APIKEY(pg),
										AUTHTKN(pg),
										RAWSIGN(pg)));
}

/*
 * Prepare the Auth part of the API call.
 */
static char
*flickr_auth_string(apr_pool_t *p, char *hash, page_data *pg)
{
        return (apr_psprintf(p, FLICKR_AUTH_STRING,
										APIKEY(pg),
										AUTHTKN(pg),
										hash,
										RAWARGS(pg)));
}

/*
 * duuplicate the string in a given
 * memory pool.
 */

static char 
*flickr_dup_string(apr_pool_t *p, char *s)
{
	return apr_pstrdup(p, s);
}

/* These macros are crocky !!! */
#define	DUP(p,s)		flickr_dup_string(p,s)

#define	APIENTRY(k,v)	apr_hash_set(svr_cfg->api_call_table,	\
									 		DUP(pchild,k),		\
									 		APR_HASH_KEY_STRING,\
											v)
#define	APIGET(h,k)		apr_hash_get(h, k,\
									 APR_HASH_KEY_STRING)
											
#define ATS(m,k,v)		apr_table_setn(m, k, v)
#define ATSD(p,m,k,v)	apr_table_setn(m, DUP(p, k), DUP(p,v))
#define ATSKD(p,m,k,v)	apr_table_setn(m, DUP(p, k), v)
#define ATSVD(p,m,k,v)	apr_table_setn(m, k, DUP(p,v))

static int
add_length(void *tbl, char *key, char *value)
{
	table_stat *t = (table_stat *) tbl;

	t->args_len += strlen(key) + strlen(value);
	t->nr_iterations++;
	
	return 1;
}

static int
flatten_table(void *data, char *key, char *value)
{
	page_data *pg = (page_data *) data;

	memcpy(SIGOFFT(pg), key, strlen(key));
	pg->offset_t += strlen(key);

	memcpy(SIGOFFT(pg), value, strlen(value));
	pg->offset_t += strlen(value);

	return 1;
}

static int
flatten_table_for_args(void *data, char *key, char *value)
{
	page_data *pg = (page_data *) data;

	memcpy(ARGOFFT(pg), key, strlen(key));
	pg->offset_t += strlen(key);

	memcpy(ARGOFFT(pg), "=", 1);
	pg->offset_t++;

	memcpy(ARGOFFT(pg), value, strlen(value));
	pg->offset_t += strlen(value);

	pg->iterations--;
	if (pg->iterations) {
		memcpy(ARGOFFT(pg), "&", 1);
		pg->offset_t++;
	}

	return 1;
}

/* ------------------------------------------------------------- */

/* ---------------- cURL invocation routines here. ------------- */

static int
flickr_memory_alloc(void *data, size_t size)
{
	mem_chunk *memory = (mem_chunk *) data;

	if (!(memory->api_response))
		memory->api_response = malloc(size);
	else
		memory->api_response = realloc(memory->api_response,
									size + memory->size);

	return (memory->api_response != NULL);
}

static size_t
curl_process_chunk(void *remote_data, size_t sz, 
									 size_t mems,
									 void *data)
{
	size_t total_size = sz * mems;
	mem_chunk *dt;

	if (flickr_memory_alloc(data, total_size)) {
		dt = (mem_chunk*) data;

		memcpy((dt->api_response) + dt->size,
				(char *)remote_data,
				total_size);
		dt->size += total_size;
		*((dt->api_response) + dt->size) = 0;
	}
	return total_size;
}

static void
flickr_request_data(mem_chunk *mem, char *api)
{

	CURL *curl_handle;

	mem->api_response = NULL;
	mem->size	 = 0;

	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();

	curl_easy_setopt(curl_handle, CURLOPT_URL, api);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION,
									 curl_process_chunk);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA,
									 (void *)mem);

	curl_easy_perform(curl_handle);
	curl_easy_cleanup(curl_handle);
}


/* ------------------------------------------------------------- */

/*
 * Return (non)configured user creds from
 * the user_cred hash.
 */
static void
*get_user(user_cred *uc, char *user)
{
	return apr_hash_get(uc->user, user, APR_HASH_KEY_STRING);
}

/*
 * Parse the request URI into username and album.
 * Create the flickr resource if the username has
 * creds given in the httpd conf file.
 * TODO: Improve this routine.
 */
static int
parse_request(request_rec *r, page_data *pg, user_cred *uc)
{
	char *request = apr_pstrdup(r->pool, r->unparsed_uri);

	if (!(pg->user = strchr(request + 1, '/')))
		return 0;

	*(pg->user) = '\0';
	pg->user++;

	if (!(pg->api_call = strchr(pg->user, '/')))
		return 0;

	*(pg->api_call) = '\0';
	pg->api_call ++;

	if (!(pg->creds = (api_key_secret *) get_user(uc, pg->user)))
		return 0;

	if ((pg->page = strchr(pg->api_call, '/'))) {
		*(pg->page) = '\0';
		pg->page++;
	}

	return 1;
}

static void
*create_per_server_config(apr_pool_t *p, server_rec *s)
{
	user_cred *uc = apr_pcalloc(p, sizeof(user_cred));
	
	uc->on_off	= 0;		/* Off by default */
	uc->user	= apr_hash_make(p);

	return uc;
}

static char
*flickr_set_on_off(cmd_parms *cmd, void *dummy, char *arg)
{
	user_cred *uc = ap_get_module_config(cmd->server->module_config,
									  	&mod_flickr);

	if (uc)
		uc->on_off = arg ? 1 : 0;

	return NULL;
}

static char
*flickr_set_user(cmd_parms *cmd, void *dummy, char *arg)
{
	api_key_secret *cred = apr_pcalloc(cmd->pool,
									  sizeof(api_key_secret));

	user_cred *uc = ap_get_module_config(cmd->server->module_config,
										&mod_flickr);

	if (uc)
		apr_hash_set(uc->user, arg, APR_HASH_KEY_STRING,
									(void *)cred);

	return NULL;
}

/*
 * Common routine for setting
 * user creds.
 */
static char
*flickr_set_var(cmd_parms *cmd, void *dummy, char *user, char *var)
{
	api_key_secret *cred;
	int offset = (int)(long) cmd->info;
	user_cred *uc = ap_get_module_config(cmd->server->module_config,
												&mod_flickr);

	if (uc) {
		if ( (cred = get_user(uc, user)) ) {
			*(const char **)((char *)cred + offset) = var;
		}
	}
	return NULL;
}


static const command_rec module_cmds[] = {
	AP_INIT_FLAG("FlickrMod", flickr_set_on_off, NULL, RSRC_CONF,
				"Enables/Disables the flickr module"),
	AP_INIT_TAKE1("FlickrUser", flickr_set_user, NULL, RSRC_CONF,
				"Username for the flickr account/URL query"),
	AP_INIT_TAKE2("FlickrKey", flickr_set_var,
				(void *)APR_OFFSETOF(api_key_secret, api_key),
				RSRC_CONF, "Username and key for the flickr user"),
	AP_INIT_TAKE2("FlickrSecret", flickr_set_var,
				(void *)APR_OFFSETOF(api_key_secret, secret),
				RSRC_CONF, "Username and secret for the account"),
	AP_INIT_TAKE2("FlickrAuth", flickr_set_var,
				(void *)APR_OFFSETOF(api_key_secret, auth_token),
				RSRC_CONF, "Username and Auth token"),
	{NULL} 
};

/* 
 * Arg. manipulation and hash generation
 * macros for APIs.
 *
 * These are used in 
 * every API that is written, so declaring
 * macros for them is a good option.
 */


/*
 * macro to compute the length of the buffer
 * needed to hold the string and the number
 * of interations;
 */

#define	GENHASHSTRING(r,pg,ts,m) \
				do { \
					apr_table_do(add_length, &ts, m, NULL);						\
					pg->raw_signature = apr_pcalloc(r->pool, ts.args_len + 1);	\
					*(pg->raw_signature + ts.args_len) = '\0';					\
					pg->offset_t = 0;											\	
				} while(0);
/*
 * flatten the signature string and
 * generate the MD5 hash from it.
 */
#define GENHASH(r,pg,m,h) \
				do { \
					apr_table_do(flatten_table, pg, m, NULL);							\
					h = flickr_md5_gen(r->pool, flickr_signature_string(r->pool, pg));	\
				} while(0);

/*
 * macros to generate the argument
 * list from the argument table.
 */
#define GENARGSTRING(r,pg,ts,m) \
				do { \
					pg->offset_t = 0;											\
					pg->iterations = ts.nr_iterations;							\
					pg->raw_args = apr_pcalloc(r->pool, SIG2ARG(ts) + 1);		\
					*(pg->raw_args + SIG2ARG(ts)) = '\0';						\
					apr_table_do(flatten_table_for_args, pg, m, NULL);			\
				} while(0);

#define GETDATA(pg, a)	flickr_request_data(&pg->mem, a);
#define DATA(pg)		pg->mem.api_response
											

/* ----------------------------------------------------------- */
/*						API CALL ROUTINES					   */

/* get photos for the user. */
static int
flickr_get_my_photos(request_rec *r, page_data *pg)
{
	char *api, *hash;
	table_stat ts = {0,0};

	apr_table_t *method_args = apr_table_make(r->pool, 3); 

	/*
	 * Fill the table with the method
	 * arguments.
	 */
	ATSD(r->pool,method_args,"method","flickr.photos.search");
	ATSKD(r->pool,method_args,"page",pg->page);
	ATSKD(r->pool,method_args,"per_page",svr_cfg->nr_photos_per_call);
	ATS(method_args,svr_cfg->user_id,svr_cfg->who);

	GENHASHSTRING(r, pg, ts, method_args);
	GENHASH(r, pg, method_args, hash);
	GENARGSTRING(r, pg, ts, method_args);

	api = flickr_auth_string(r->pool, hash, pg);

#ifdef DEBUG
	ap_log_error(APLOG_MARK, APLOG_CRIT, 0, r->server,
				"API: %s, child pid: %d", api, getpid());
#endif

	GETDATA(pg, api);

	if (DATA(pg)) {
		apr_pool_cleanup_register(r->pool, pg->mem.api_response,
									      free,
										  apr_pool_cleanup_null);

		return FLICKR_STATUS_OK;
	}

	return FLICKR_STATUS_ERR;
}

/*
 * get users photosets.
 */
static int
flickr_get_my_sets(request_rec *r, page_data *pg)
{
	char *api, *hash;
	table_stat ts = {0,0};

	apr_table_t *method_args = apr_table_make(r->pool, 3); 

	/*
	 * Fill the table with the method
	 * arguments.
	 */
	ATSD(r->pool,method_args,"method","flickr.photosets.getList");
	GENHASHSTRING(r, pg, ts, method_args);
	GENHASH(r, pg, method_args, hash);
	GENARGSTRING(r, pg, ts, method_args);

	api = flickr_auth_string(r->pool, hash, pg);

#ifdef DEBUG
	ap_log_error(APLOG_MARK, APLOG_CRIT, 0, r->server,
				"API: %s, child pid: %d", api, getpid());
#endif

	GETDATA(pg, api);

	if (DATA(pg)) {
		apr_pool_cleanup_register(r->pool, pg->mem.api_response,
									      free,
										  apr_pool_cleanup_null);

		return FLICKR_STATUS_OK;
	}

	return FLICKR_STATUS_ERR;
}

/* ----------------------------------------------------------- */

/*
 * Flickr URL handler
 */
static int
flickr_handler(request_rec *r)
{
	if (!r->handler || strcmp(r->handler, "flickr-handler") != 0)
		return DECLINED;

	user_cred *uc = ap_get_module_config(r->server->module_config,
										&mod_flickr);

	if (!uc->on_off)
		return DECLINED;

	page_data *pg = apr_pcalloc(r->pool, sizeof(page_data));

	if (!parse_request(r, pg, uc)) {
#ifdef DEBUG
		ap_log_error(APLOG_MARK, APLOG_CRIT, 0, r->server,
					"User/api name invalid!!!");
#endif
		return DECLINED;
	}

	int (*fn) (request_rec *, page_data *);

	if ( (fn = APIGET(svr_cfg->api_call_table, APINAM(pg))) ) {

		if ((*fn) (r, pg)) {
			ap_set_content_type(r, "application/xml");
			ap_rputs(DATA(pg), r);
			return OK;
		} else {
			ap_log_error(APLOG_MARK, APLOG_CRIT, 0, r->server,
						"API call for [%s] failed to get data !!!",
						APINAM(pg));
			return HTTP_INTERNAL_SERVER_ERROR;
		}
	} else {
		ap_log_error(APLOG_MARK, APLOG_CRIT, 0, r->server,
					"API entry for call: [%s] missing !!!",
					APINAM(pg));
		return HTTP_NOT_FOUND;
	}
}

/*
 * child init hook
 */
static void
flickr_child_init(apr_pool_t *pchild, server_rec *s)
{
	svr_cfg = apr_pcalloc(pchild, sizeof(svr_constants));

	svr_cfg->nr_display_photos  = apr_itoa(pchild,
										   FLICKR_NR_DISPLAY_PHOTOS);
	svr_cfg->nr_photos_per_call = apr_itoa(pchild,
										   FLICKR_NR_PHOTOS_PER_CALL);
	svr_cfg->user_id 			= apr_pstrdup(pchild, "user_id");
	svr_cfg->who				= apr_pstrdup(pchild, "me"); 

	/* initialize the API call table. */
	svr_cfg->api_call_table = apr_hash_make(pchild);

	/* API call entries. */
	{
		APIENTRY("getMyPhotos", flickr_get_my_photos);
		APIENTRY("getMySets",	flickr_get_my_sets);
	}
}

static void
register_hooks(apr_pool_t *p)
{
	ap_hook_handler(flickr_handler, NULL, NULL, APR_HOOK_MIDDLE);
	ap_hook_child_init(flickr_child_init, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA mod_flickr = {
	STANDARD20_MODULE_STUFF,
	NULL,						/* per dir. config		    */
	NULL,						/* merge per dir. config    */
	create_per_server_config,   /* per server config		*/
	NULL,						/* merge per server config	*/
	module_cmds,				/* module's commands table	*/
	register_hooks				/* module's hook table		*/
};



