#include "ccache.h"

#ifdef HAVE_LIBCOUCHBASE

#include <libcouchbase/couchbase.h>

/* couchbase instance */
static lcb_t cb = NULL;

static void storage_callback(lcb_t instance,
                             const void *cookie, lcb_storage_t op,
                             lcb_error_t err, const lcb_store_resp_t *resp);

static void get_callback(lcb_t instance,
                         const void *cookie, lcb_error_t err,
                         const lcb_get_resp_t *resp);

typedef struct cc_blob {
	void *data;
	lcb_size_t len;
} cc_blob_t;

int cc_couchbase_init(char *conf)
{
	struct lcb_create_st cropts = { 0 };
	cropts.version = 3;
	cropts.v.v3.connstr = conf; /* "couchbase://localhost/default" */
	lcb_error_t err;

	err = lcb_create(&cb, &cropts);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't create instance: %s",
		       lcb_strerror(NULL, err));
		return err;
	}

	lcb_connect(cb);
	lcb_wait(cb);
	err = lcb_get_bootstrap_status(cb);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't bootstrap: %s",
		       lcb_strerror(NULL, err));
		return err;
	}

	lcb_set_store_callback(cb, storage_callback);
	lcb_set_get_callback(cb, get_callback);

	return 0;
}

void storage_callback(lcb_t instance, const void *cookie, lcb_storage_t op,
                      lcb_error_t err, const lcb_store_resp_t *resp)
{
	(void) instance; (void) cookie; (void) op;
	if (err == LCB_SUCCESS) {
		cc_log("CB Stored %.*s", (int)resp->v.v0.nkey, (char *)resp->v.v0.key);
	}
}

int cc_couchbase_set(const char *key, const char *type, const char *data,
                     size_t len)
{
	lcb_store_cmd_t scmd = { 0 };
	const lcb_store_cmd_t *scmdlist = &scmd;
	lcb_error_t err;
	char *buf;
	if (cb == NULL)
		return -1;
	buf = format("%s.%s", key, type);
	scmd.v.v0.key = buf;
	scmd.v.v0.nkey = strlen(buf);
	scmd.v.v0.bytes = data;
	scmd.v.v0.nbytes = len;
	scmd.v.v0.operation = LCB_SET;
	err = lcb_store(cb, NULL, 1, &scmdlist);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't schedule storage operation!");
		return err;
	}
	lcb_wait(cb); //storage_callback is invoked here
	free(buf);
	return 0;
}

void get_callback(lcb_t instance, const void *cookie, lcb_error_t err,
                  const lcb_get_resp_t *resp)
{
	(void) instance;
	if (err == LCB_SUCCESS) {
		cc_log("CB Retrieved %.*s", (int)resp->v.v0.nkey, (char *)resp->v.v0.key);
		((cc_blob_t *) cookie)->data = (void *) resp->v.v0.bytes;
		((cc_blob_t *) cookie)->len = resp->v.v0.nbytes;
	} else if (cookie) {
		cc_log("CB Miss %.*s", (int)resp->v.v0.nkey, (char *)resp->v.v0.key);
		((cc_blob_t *) cookie)->data = NULL;
		((cc_blob_t *) cookie)->len = 0;
	}
}

int cc_couchbase_get(const char *key, const char *type, char **data,
                     size_t *len)
{
	cc_blob_t blob;
	lcb_get_cmd_t gcmd = { 0 };
	const lcb_get_cmd_t *gcmdlist = &gcmd;
	lcb_error_t err;
	char *buf;
	if (cb == NULL)
		return -1;
	buf = format("%s.%s", key, type);
	gcmd.v.v0.key = buf;
	gcmd.v.v0.nkey = strlen(buf);
	err = lcb_get(cb, &blob, 1, &gcmdlist);
	if (err != LCB_SUCCESS) {
		cc_log("Couldn't schedule get operation!");
		return err;
	}
	lcb_wait(cb); // get_callback is invoked here
	free(buf);

	*data = blob.data;
	*len = blob.len;
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
