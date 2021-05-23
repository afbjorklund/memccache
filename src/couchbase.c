#include "ccache.h"

#ifdef HAVE_LIBCOUCHBASE

#include <libcouchbase/couchbase.h>

/* couchbase instance */
static lcb_INSTANCE * cb = NULL;

static void store_callback(lcb_INSTANCE * instance, int cbtype,
                           const lcb_RESPBASE *rb);

static void get_callback(lcb_INSTANCE * instance, int cbtype,
                         const lcb_RESPBASE *rb);

static void remove_callback(lcb_INSTANCE * instance, int cbtype,
                            const lcb_RESPBASE *rb);


typedef struct cc_blob {
	void *data;
	lcb_size_t len;
} cc_blob_t;

int cc_couchbase_init(char *conf)
{
	const char *username = getenv("CCACHE_COUCHBASE_USERNAME");
	const char *password = getenv("CCACHE_COUCHBASE_PASSWORD"); /* Note: unsafe */
	lcb_STATUS err;

	lcb_CREATEOPTS *cropts;
	err = lcb_createopts_create(&cropts, LCB_TYPE_BUCKET);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't create createopts: %s",
		       lcb_strerror_short(err));
		return err;
	}

	/* conf = "couchbase://localhost/default" */
	lcb_createopts_connstr(cropts, conf, strlen(conf));
	if (username && password) {
		lcb_createopts_credentials(cropts,
		                           username, strlen(username),
		                           password, strlen(password));
	}

	err = lcb_create(&cb, cropts);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't create instance: %s",
		       lcb_strerror_short(err));
		return err;
	}

	lcb_createopts_destroy(cropts);

	lcb_connect(cb);
	lcb_wait(cb, LCB_WAIT_DEFAULT);
	err = lcb_get_bootstrap_status(cb);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't bootstrap: %s",
		       lcb_strerror_short(err));
		return err;
	}

	lcb_install_callback(cb, LCB_CALLBACK_STORE, store_callback);
	lcb_install_callback(cb, LCB_CALLBACK_GET, get_callback);
	lcb_install_callback(cb, LCB_CALLBACK_REMOVE, remove_callback);

	return 0;
}

static void store_callback(lcb_INSTANCE * instance, int cbtype,
                           const lcb_RESPBASE *rb)
{
	const lcb_RESPSTORE *resp = (const lcb_RESPSTORE*)rb;
	(void) instance; (void) cbtype;
	lcb_STATUS err = lcb_respstore_status(resp);
	if (err == LCB_SUCCESS) {
		size_t nkey;
		const char *key;
		lcb_respstore_key(resp, &key, &nkey);
		cc_log("CB Stored %.*s", (int)nkey, (char *)key);
	} else {
		cc_log("CB Fail %s", lcb_strerror_short(err));
	}
}

int cc_couchbase_set(const char *key, const char *type, const char *data,
                     size_t len)
{
	lcb_CMDSTORE* scmd;
	lcb_STATUS err;
	char *buf;
	if (cb == NULL)
		return -1;
	buf = format("%s.%s", key, type);
	lcb_cmdstore_create(&scmd, LCB_STORE_UPSERT);
	lcb_cmdstore_key(scmd, buf, strlen(buf));
	lcb_cmdstore_value(scmd, data, len);
	err = lcb_store(cb, NULL, scmd);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't schedule storage operation!");
		//cc_log("CB err %s", lcb_strerror_long(err));
		return err;
	}
	lcb_wait(cb, LCB_WAIT_NOCHECK); //storage_callback is invoked here
	free(buf);
	return 0;
}

void get_callback(lcb_INSTANCE * instance, int cbtype,
                         const lcb_RESPBASE *rb)
{
	const lcb_RESPGET *resp = (const lcb_RESPGET*)rb;
	void *cookie;
	lcb_respget_cookie(resp, &cookie);
	(void) instance; (void) cbtype;
	lcb_STATUS err = lcb_respget_status(resp);
	if (err == LCB_SUCCESS) {
		size_t nkey;
		const char *key;
		lcb_respget_key(resp, &key, &nkey);
		cc_log("CB Retrieved %.*s", (int)nkey, (char *)key);
		size_t nbytes;
		const char *bytes;
		lcb_respget_value(resp, &bytes, &nbytes);
		((cc_blob_t *) cookie)->data = (void *) bytes;
		((cc_blob_t *) cookie)->len = nbytes;
	} else if (cookie) {
		size_t nkey;
		const char *key;
		lcb_respget_key(resp, &key, &nkey);
		cc_log("CB Miss %.*s", (int)nkey, (char *)key);
		//cc_log("CB err %s", lcb_strerror_long(err));
		((cc_blob_t *) cookie)->data = NULL;
		((cc_blob_t *) cookie)->len = 0;
	}
}

int cc_couchbase_get(const char *key, const char *type, char **data,
                     size_t *len)
{
	cc_blob_t blob;
	lcb_CMDGET* gcmd;
	lcb_STATUS err;
	char *buf;
	if (cb == NULL)
		return -1;
	buf = format("%s.%s", key, type);
	lcb_cmdget_create(&gcmd);
	lcb_cmdget_key(gcmd, buf, strlen(buf));
	err = lcb_get(cb, &blob, gcmd);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't schedule get operation!");
		return err;
	}
	lcb_wait(cb, LCB_WAIT_NOCHECK); // get_callback is invoked here
	free(buf);

	*data = blob.data;
	*len = blob.len;
	return 0;
}

void remove_callback(lcb_INSTANCE * instance, int cbtype,
                         const lcb_RESPBASE *rb)
{
	const lcb_RESPREMOVE *resp = (const lcb_RESPREMOVE*)rb;
	(void) instance; (void) cbtype;
	lcb_STATUS err = lcb_respremove_status(resp);
	if (err == LCB_SUCCESS) {
		size_t nkey;
		const char *key;
		lcb_respremove_key(resp, &key, &nkey);
		cc_log("CB Removed %.*s", (int)nkey, (char *)key);
	} else {
		size_t nkey;
		const char *key;
		lcb_respremove_key(resp, &key, &nkey);
		cc_log("CB Remove %.*s", (int)nkey, (char *)key);
		//cc_log("CB err %s", lcb_strerror_long(err));
	}
}

int cc_couchbase_rm(const char *key, const char *type)
{
	lcb_CMDREMOVE* rcmd;
	lcb_STATUS err;
	char *buf;
	if (cb == NULL)
		return -1;
	buf = format("%s.%s", key, type);
	lcb_cmdremove_create(&rcmd);
	lcb_cmdremove_key(rcmd, buf, strlen(buf));
	err = lcb_remove(cb, NULL, rcmd);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't schedule remove operation!");
		return err;
	}
	lcb_wait(cb, LCB_WAIT_NOCHECK); // remove_callback is invoked here
	return 0;
}

int cc_couchbase_release(void)
{
	if (cb == NULL)
		return 0;
	lcb_destroy(cb);
	return 1;
}

#endif
