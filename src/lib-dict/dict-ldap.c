/* Copyright (c) 2016 Dovecot authors, see the included COPYING memcached */

#include "lib.h"
#include "array.h"
#include "module-dir.h"
#include "str.h"
#include "istream.h"
#include "ostream.h"
#include "var-expand.h"
#include "connection.h"
#include "llist.h"
#include "ldap-client.h"
#include "dict.h"
#include "dict-private.h"
#include "dict-ldap-settings.h"

struct ldap_dict;

struct dict_ldap_op {
	struct ldap_dict *dict;
	const struct dict_ldap_map *map;
	pool_t pool;
	unsigned long txid;
	struct dict_lookup_result res;
	dict_lookup_callback_t *callback;
	void *callback_ctx;
};

struct ldap_dict {
	struct dict dict;
	struct dict_ldap_settings *set;

	const char *uri;
	const char *username;
	const char *base_dn;
	enum ldap_scope scope;

	pool_t pool;

	struct ldap_client *client;
	struct ioloop *ioloop, *prev_ioloop;

	unsigned long last_txid;
	unsigned int pending;

	struct ldap_dict *prev,*next;
};

static
struct ldap_dict *ldap_dict_list;

static
void ldap_dict_lookup_async(struct dict *dict, const char *key,
			     dict_lookup_callback_t *callback, void *context);


static bool
dict_ldap_map_match(const struct dict_ldap_map *map, const char *path,
		   ARRAY_TYPE(const_string) *values, unsigned int *pat_len_r,
		   unsigned int *path_len_r, bool partial_ok, bool recurse)
{
	const char *path_start = path;
	const char *pat, *attribute, *p;
	unsigned int len;

	array_clear(values);
	pat = map->pattern;
	while (*pat != '\0' && *path != '\0') {
		if (*pat == '$') {
			/* variable */
			pat++;
			if (*pat == '\0') {
				/* pattern ended with this variable,
				   it'll match the rest of the path */
				len = strlen(path);
				if (partial_ok) {
					/* iterating - the last field never
					   matches fully. if there's a trailing
					   '/', drop it. */
					pat--;
					if (path[len-1] == '/') {
						attribute = t_strndup(path, len-1);
						array_append(values, &attribute, 1);
					} else {
						array_append(values, &path, 1);
					}
				} else {
					array_append(values, &path, 1);
					path += len;
				}
				*path_len_r = path - path_start;
				*pat_len_r = pat - map->pattern;
				return TRUE;
			}
			/* pattern matches until the next '/' in path */
			p = strchr(path, '/');
			if (p != NULL) {
				attribute = t_strdup_until(path, p);
				array_append(values, &attribute, 1);
				path = p;
			} else {
				/* no '/' anymore, but it'll still match a
				   partial */
				array_append(values, &path, 1);
				path += strlen(path);
				pat++;
			}
		} else if (*pat == *path) {
			pat++;
			path++;
		} else {
			return FALSE;
		}
	}

	*path_len_r = path - path_start;
	*pat_len_r = pat - map->pattern;

	if (*pat == '\0')
		return *path == '\0';
	else if (!partial_ok)
		return FALSE;
	else {
		/* partial matches must end with '/'. */
		if (pat != map->pattern && pat[-1] != '/')
			return FALSE;
		/* if we're not recursing, there should be only one $variable
		   left. */
		if (recurse)
			return TRUE;
		return pat[0] == '$' && strchr(pat, '/') == NULL;
	}
}

static const struct dict_ldap_map *
ldap_dict_find_map(struct ldap_dict *dict, const char *path,
		  ARRAY_TYPE(const_string) *values)
{
	const struct dict_ldap_map *maps;
	unsigned int i, count, len;

	t_array_init(values, dict->set->max_attribute_count);
	maps = array_get(&dict->set->maps, &count);
	for (i = 0; i < count; i++) {
		if (dict_ldap_map_match(&maps[i], path, values,
				       &len, &len, FALSE, FALSE))
			return &maps[i];
	}
	return NULL;
}

static
int dict_ldap_connect(struct ldap_dict *dict, const char **error_r)
{
	struct ldap_client_settings set;
	memset(&set, 0, sizeof(set));
	set.uri = dict->set->uri;
	set.bind_dn = dict->set->bind_dn;
	set.password = dict->set->password;
	set.timeout_secs = dict->set->timeout;
	set.max_idle_time_secs = dict->set->max_idle_time;
	set.debug = dict->set->debug;
	set.require_ssl = dict->set->require_ssl;
	return ldap_client_init(&set, &dict->client, error_r);
}

static
const char* ldap_dict_build_query(struct ldap_dict *dict, const struct dict_ldap_map *map, ARRAY_TYPE(const_string) *values, bool priv)
{
	const char *template;
	ARRAY(struct var_expand_table) exp;
	struct var_expand_table entry;
	string_t *query = t_str_new(64);

	t_array_init(&exp, 8);
	entry.key = '\0';
	entry.value = dict->username;
	entry.long_key = "username";
	array_append(&exp, &entry, 1);

	if (priv) {
		template = t_strdup_printf("(&(%s=%s)%s)", map->username_attribute, "%{username}", map->filter);
	} else {
		template = map->filter;
	}

	for(size_t i = 0; i < array_count(values) && i < array_count(&(map->ldap_attributes)); i++) {
		struct var_expand_table entry;
		entry.value = *array_idx(values, i);
		entry.long_key = *array_idx(&(map->ldap_attributes), i);
		array_append(&exp, &entry, 1);
	}

	array_append_zero(&exp);

	var_expand(query, template, array_idx(&exp, 0));

	return str_c(query);
}

static
int ldap_dict_create(struct dict *dict_driver, const char *uri,
		     const struct dict_settings *set,
		     struct dict **dict_r, const char **error_r)
{
	pool_t pool = pool_alloconly_create("ldap dict", 2048);
	struct ldap_dict *dict = p_new(pool, struct ldap_dict, 1);
	dict->pool = pool;
	dict->dict = *dict_driver;
	dict->username = p_strdup(pool, set->username);
	dict->uri = p_strdup(pool, uri);
	dict->set = dict_ldap_settings_read(pool, uri, error_r);

	if (dict->set == NULL) {
		pool_unref(&pool);
		return -1;
	}

	if (dict_ldap_connect(dict, error_r) < 0) {
		pool_unref(&pool);
		return -1;
	}

	*dict_r = (struct dict*)dict;
	*error_r = NULL;

	DLLIST_PREPEND(&ldap_dict_list, dict);

	return 0;
}

static
int ldap_dict_init(struct dict *dict_driver, const char *uri,
		   const struct dict_settings *set,
		   struct dict **dict_r, const char **error_r)
{
	/* reuse possible existing entry */
	for(struct ldap_dict *ptr = ldap_dict_list;
	    ptr != NULL;
	    ptr = ptr->next) {
		if (strcmp(ptr->uri, uri) == 0) {
			*dict_r = (struct dict*)ptr;
			return 0;
		}
	}
	return ldap_dict_create(dict_driver, uri, set, dict_r, error_r);
}

static
void ldap_dict_deinit(struct dict *dict ATTR_UNUSED) {
}

static
int ldap_dict_wait(struct dict *dict) {
	struct ldap_dict *ctx = (struct ldap_dict *)dict;

	i_assert(ctx->ioloop == NULL);

	ctx->prev_ioloop = current_ioloop;
	ctx->ioloop = io_loop_create();
	ldap_client_switch_ioloop(ctx->client);

	do {
		io_loop_run(current_ioloop);
	} while (ctx->pending > 0);

	io_loop_set_current(ctx->prev_ioloop);
	ldap_client_switch_ioloop(ctx->client);
	io_loop_set_current(ctx->ioloop);
	io_loop_destroy(&ctx->ioloop);
	ctx->prev_ioloop = NULL;

	return 0;
}

static
void ldap_dict_lookup_done(const struct dict_lookup_result *result, void *ctx)
{
	struct dict_lookup_result *res = ctx;
	*res = *result;
}

static void
ldap_dict_lookup_callback(struct ldap_result *result, struct dict_ldap_op *op)
{
	pool_t pool = op->pool;
	struct ldap_search_iterator *iter;
	const struct ldap_entry *entry;

	op->dict->pending--;

	if (ldap_result_has_failed(result)) {
		op->res.ret = -1;
		op->res.error = ldap_result_get_error(result);
	} else {
		iter = ldap_search_iterator_init(result);
		entry = ldap_search_iterator_next(iter);
		if (entry != NULL) {
			i_debug("ldap_dict_lookup_callback got dn %s", ldap_entry_dn(entry));
			/* try extract value */
			const char *const *values = ldap_entry_get_attribute(entry, op->map->value_attribute);
			if (values != NULL) {
				i_debug("ldap_dict_lookup_callback got attribute %s", op->map->value_attribute);
				op->res.ret = 1;
				op->res.value = p_strdup(op->pool, values[0]);
			} else {
				i_debug("ldap_dict_lookup_callback dit not get attribute %s", op->map->value_attribute);
				op->res.value = NULL;
			}
		}
		ldap_search_iterator_deinit(&iter);
	}
	op->callback(&(op->res), op->callback_ctx);
	pool_unref(&pool);
}

static
int ldap_dict_lookup(struct dict *dict, pool_t pool,
		      const char *key, const char **value_r)
{
	struct dict_lookup_result res;
	pool_t orig_pool = pool;
	int ret;

	T_BEGIN {
		ldap_dict_lookup_async(dict, key, ldap_dict_lookup_done, &res);

		if ((ret = ldap_dict_wait(dict)) == 0) {
			if (res.ret == 0) {
				*value_r = p_strdup(orig_pool, res.value);
			} else ret = res.ret;
		}
	} T_END;
	return ret;
}

/*
static
struct dict_iterate_context *ldap_dict_iterate_init(struct dict *dict,
				const char *const *paths,
				enum dict_iterate_flags flags)
{
	return NULL;
}

static
bool ldap_dict_iterate(struct dict_iterate_context *ctx,
			const char **key_r, const char **value_r)
{
	return FALSE;
}

static
int ldap_dict_iterate_deinit(struct dict_iterate_context *ctx)
{
	return -1;
}

static
struct dict_transaction_context ldap_dict_transaction_init(struct dict *dict);

static
int ldap_dict_transaction_commit(struct dict_transaction_context *ctx,
				  bool async,
				  dict_transaction_commit_callback_t *callback,
				  void *context);
static
void ldap_dict_transaction_rollback(struct dict_transaction_context *ctx);

static
void ldap_dict_set(struct dict_transaction_context *ctx,
		    const char *key, const char *value);
static
void ldap_dict_unset(struct dict_transaction_context *ctx,
		      const char *key);
static
void ldap_dict_append(struct dict_transaction_context *ctx,
		       const char *key, const char *value);
static
void ldap_dict_atomic_inc(struct dict_transaction_context *ctx,
			   const char *key, long long diff);
*/

static
void ldap_dict_lookup_async(struct dict *dict, const char *key,
			     dict_lookup_callback_t *callback, void *context)
{
	struct ldap_search_input input;
	struct ldap_dict *ctx = (struct ldap_dict*)dict;
	struct dict_ldap_op *op;
	pool_t oppool = pool_alloconly_create("ldap dict lookup", 64);
	op = p_new(oppool, struct dict_ldap_op, 1);
	op->pool = oppool;
	op->dict = ctx;
	op->callback = callback;
	op->callback_ctx = context;
	op->txid = ctx->last_txid++;

	/* key needs to be transformed into something else */
	ARRAY_TYPE(const_string) values;
	T_BEGIN {
		const char *attributes[2] = {0, 0};
		t_array_init(&values, 8);
		const struct dict_ldap_map *map = ldap_dict_find_map(ctx, key, &values);

		if (map != NULL) {
			op->map = map;
			attributes[0] = map->value_attribute;
			/* build lookup */
			memset(&input, 0, sizeof(input));
			input.base_dn = map->base_dn;
			input.scope = map->scope_val;
			input.filter = ldap_dict_build_query(ctx, map, &values, strncmp(key, DICT_PATH_PRIVATE, strlen(DICT_PATH_PRIVATE))==0);
			input.attributes = attributes;
			input.timeout_secs = ctx->set->timeout;
			ctx->pending++;
			ldap_search_start(ctx->client, &input, ldap_dict_lookup_callback, op);
		} else {
			op->res.error = "no such key";
			callback(&(op->res), context);
			pool_unref(&oppool);
		}
	} T_END;
}

struct dict dict_driver_ldap = {
	.name = "ldap",
	{
		ldap_dict_init,
		ldap_dict_deinit,
		ldap_dict_wait,
		ldap_dict_lookup,
		NULL, /*ldap_dict_iterate_init,*/
		NULL, /*ldap_dict_iterate,*/
		NULL, /*ldap_dict_iterate_deinit,*/
		NULL, /*ldap_transaction_init,*/
		NULL, /*ldap_transaction_commit,*/
		NULL, /*ldap_transaction_rollback,*/
		NULL, /*ldap_set,*/
		NULL, /*ldap_unset,*/
		NULL, /*ldap_append,*/
		NULL, /*ldap_atomic_inc,*/
		ldap_dict_lookup_async
	}
};

void dict_ldap_init(struct module *module ATTR_UNUSED);
void dict_ldap_deinit(void);

void dict_ldap_init(struct module *module ATTR_UNUSED)
{
	dict_driver_register(&dict_driver_ldap);
	ldap_dict_list = NULL;
}

void dict_ldap_deinit(void)
{
	dict_driver_unregister(&dict_driver_ldap);
	/* destroy all server connections */
	struct ldap_dict *ptr = ldap_dict_list;
	ldap_dict_list = NULL;

	while(ptr != NULL) {
		ldap_client_deinit(&(ptr->client));
		pool_t pool = ptr->pool;
		ptr = ptr->next;
		pool_unref(&pool);
	}
}

const char *dict_ldap_plugin_dependencies[] = { NULL };