/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_module.c
 *
 *    Description:  nginx module for shared custom counters
 *
 *        Version:  4.0
 *        Created:  04.08.2016 14:00:10
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#include <ngx_config.h>

#include "ngx_http_custom_counters_module.h"
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
#include "ngx_http_custom_counters_persistency.h"
#endif
#include "ngx_http_custom_counters_histogram.h"


static time_t  ngx_http_cnt_start_time;
static time_t  ngx_http_cnt_start_time_reload;

static const ngx_str_t  ngx_http_cnt_shm_name_prefix =
    ngx_string("custom_counters_");


typedef enum {
    ngx_http_cnt_op_set,
    ngx_http_cnt_op_inc,
    ngx_http_cnt_op_undo
} ngx_http_cnt_op_e;


typedef struct {
    ngx_int_t                   self;
    ngx_uint_t                  negative;
} ngx_http_cnt_rt_var_data_t;


typedef struct {
    ngx_http_cnt_op_e           op;
    ngx_int_t                   self;
    ngx_int_t                   idx;
    ngx_int_t                   value;
    ngx_array_t                 rt_vars;
    ngx_uint_t                  early;
} ngx_http_cnt_data_t;


typedef struct {
    ngx_int_t                   idx;
    ngx_array_t                *range;
} ngx_http_cnt_map_range_index_data_t;


typedef struct {
    double                      value;
    ngx_str_t                   s_value;
} ngx_http_cnt_range_boundary_data_t;


typedef struct {
    ngx_array_t                *cnt_sets;
    ngx_uint_t                  cnt_set;
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    ngx_str_t                   persistent_collection;
    jsmntok_t                  *persistent_collection_tok;
    int                         persistent_collection_size;
#endif
} ngx_http_cnt_shm_data_t;


typedef struct {
    ngx_array_t                 cnt_data;
} ngx_http_cnt_loc_conf_t;


static ngx_int_t ngx_http_cnt_add_vars(ngx_conf_t *cf);
static ngx_int_t ngx_http_cnt_init(ngx_conf_t *cf);
static void *ngx_http_cnt_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_cnt_create_srv_conf(ngx_conf_t *cf);
static void *ngx_http_cnt_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_cnt_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_cnt_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_cnt_init_module(ngx_cycle_t *cycle);
static void ngx_http_cnt_exit_master(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cnt_shm_init(ngx_shm_zone_t *shm_zone, void *data);
static ngx_int_t ngx_http_cnt_get_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static ngx_int_t ngx_http_cnt_collection(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void ngx_http_cnt_set_collection_buf_len(ngx_http_cnt_main_conf_t *mcf);
static ngx_int_t ngx_http_cnt_uptime(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_cnt_get_range_index(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static char *ngx_http_cnt_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_early_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_counter_set_id(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_map_range_index(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_merge(ngx_conf_t *cf, ngx_array_t *dst,
    ngx_http_cnt_data_t *cnt_data);
static ngx_inline ngx_int_t ngx_http_cnt_phase_handler_impl(
    ngx_http_request_t *r, ngx_uint_t early);
static ngx_int_t ngx_http_cnt_rewrite_phase_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_cnt_log_phase_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_cnt_update(ngx_http_request_t *r, ngx_uint_t early);


static ngx_command_t  ngx_http_cnt_commands[] = {

    { ngx_string("counter"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE123,
      ngx_http_cnt_counter,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("early_counter"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE23,
      ngx_http_cnt_early_counter,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("counter_set_id"),
      NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_http_cnt_counter_set_id,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("counters_survive_reload"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_cnt_srv_conf_t, survive_reload),
      NULL },
    { ngx_string("histogram"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE23,
      ngx_http_cnt_histogram,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("map_range_index"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
      ngx_http_cnt_map_range_index,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    { ngx_string("counters_persistent_storage"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE12,
      ngx_http_cnt_counters_persistent_storage,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },
#endif
    { ngx_string("display_unreachable_counter_as"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_cnt_srv_conf_t, unreachable_cnt_mark),
      NULL },

      ngx_null_command
};


static ngx_http_variable_t  ngx_http_cnt_vars[] =
{
    { ngx_string("cnt_collection"), NULL, ngx_http_cnt_collection, 0, 0, 0 },
    { ngx_string("cnt_uptime"), NULL, ngx_http_cnt_uptime, 0, 0, 0 },
    { ngx_string("cnt_uptime_reload"), NULL, ngx_http_cnt_uptime, 1, 0, 0 },

    { ngx_null_string, NULL, NULL, 0, 0, 0 }
};


static ngx_http_module_t  ngx_http_cnt_module_ctx = {
    ngx_http_cnt_add_vars,                   /* preconfiguration */
    ngx_http_cnt_init,                       /* postconfiguration */

    ngx_http_cnt_create_main_conf,           /* create main configuration */
    NULL,                                    /* init main configuration */

    ngx_http_cnt_create_srv_conf,            /* create server configuration */
    ngx_http_cnt_merge_srv_conf,             /* merge server configuration */

    ngx_http_cnt_create_loc_conf,            /* create location configuration */
    ngx_http_cnt_merge_loc_conf              /* merge location configuration */
};


ngx_module_t  ngx_http_custom_counters_module = {
    NGX_MODULE_V1,
    &ngx_http_cnt_module_ctx,                /* module context */
    ngx_http_cnt_commands,                   /* module directives */
    NGX_HTTP_MODULE,                         /* module type */
    NULL,                                    /* init master */
    ngx_http_cnt_init_module,                /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    NULL,                                    /* exit process */
    ngx_http_cnt_exit_master,                /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_cnt_add_vars(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_cnt_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_init(ngx_conf_t *cf)
{
    ngx_uint_t                   i, j;
    ngx_http_core_main_conf_t   *cmcf;
    ngx_http_core_srv_conf_t   **cscfp;
    ngx_http_cnt_main_conf_t    *mcf;
    ngx_http_cnt_srv_conf_t     *scf;
    ngx_str_t                    cnt_set_id;
    ngx_http_cnt_set_t          *cnt_sets;
    ngx_http_handler_pt         *h;
    time_t                       now;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    cscfp = cmcf->servers.elts;
    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_custom_counters_module);
    cnt_sets = mcf->cnt_sets.elts;

    for (i = 0; i < cmcf->servers.nelts; i++) {
        scf = cscfp[i]->ctx->srv_conf[
                ngx_http_custom_counters_module.ctx_index];
        cnt_set_id = scf->cnt_set_id;
        if (scf->cnt_set != NGX_CONF_UNSET_UINT
            || (cnt_set_id.len == 0 && cscfp[i]->server_names.nelts == 0))
        {
            continue;
        }
        if (cnt_set_id.len == 0) {
            cnt_set_id = ((ngx_http_server_name_t *)
                            cscfp[i]->server_names.elts)[
                                cscfp[i]->server_names.nelts - 1].name;
        }
        for (j = 0; j < mcf->cnt_sets.nelts; j++) {
            if (cnt_sets[j].name.len == cnt_set_id.len
                && ngx_strncmp(cnt_sets[j].name.data, cnt_set_id.data,
                               cnt_set_id.len) == 0)
            {
                scf->cnt_set = j;
                break;
            }
        }
    }

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_cnt_rewrite_phase_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_cnt_log_phase_handler;

    ngx_http_cnt_set_collection_buf_len(mcf);

    now = ngx_time();

    if (ngx_http_cnt_start_time == 0) {
        ngx_http_cnt_start_time = now;
    }
    ngx_http_cnt_start_time_reload = now;

    return NGX_OK;
}


static void *
ngx_http_cnt_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_cnt_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cnt_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&mcf->cnt_sets, cf->pool, 1,
                       sizeof(ngx_http_cnt_set_t)) != NGX_OK)
    {
        return NULL;
    }

    return mcf;
}


static void *
ngx_http_cnt_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_cnt_srv_conf_t  *scf;

    scf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cnt_srv_conf_t));
    if (scf == NULL) {
        return NULL;
    }

    scf->cnt_set = NGX_CONF_UNSET_UINT;
    scf->survive_reload = NGX_CONF_UNSET;

    return scf;
}


static void *
ngx_http_cnt_create_loc_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_cnt_loc_conf_t));
}


static char *
ngx_http_cnt_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_cnt_srv_conf_t   *prev = parent;
    ngx_http_cnt_srv_conf_t   *conf = child;

    ngx_http_cnt_main_conf_t  *mcf;
    ngx_http_cnt_set_t        *cnt_sets;

    ngx_conf_merge_uint_value(conf->cnt_set, prev->cnt_set,
                              NGX_CONF_UNSET_UINT);
    ngx_conf_merge_str_value(conf->cnt_set_id, prev->cnt_set_id, "");
    ngx_conf_merge_str_value(conf->unreachable_cnt_mark,
                             prev->unreachable_cnt_mark, "");
    ngx_conf_merge_value(conf->survive_reload, prev->survive_reload, 0);

    if (conf->survive_reload && conf->cnt_set != NGX_CONF_UNSET_UINT) {
        mcf = ngx_http_conf_get_module_main_conf(cf,
                                            ngx_http_custom_counters_module);
        cnt_sets = mcf->cnt_sets.elts;
        cnt_sets[conf->cnt_set].survive_reload = 1;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_cnt_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_cnt_loc_conf_t     *prev = parent;
    ngx_http_cnt_loc_conf_t     *conf = child;

    ngx_uint_t                   i, j, size;
    ngx_http_cnt_data_t         *cnt_data, *prev_cnt_data;
    ngx_array_t                  child_data;
    ngx_http_cnt_rt_var_data_t  *rt_var;

    if (prev->cnt_data.nelts == 0) {
        return NGX_CONF_OK;
    }

    size = ngx_max(prev->cnt_data.nelts, conf->cnt_data.nelts);
    if (ngx_array_init(&child_data, cf->pool, size,
                       sizeof(ngx_http_cnt_data_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    prev_cnt_data = prev->cnt_data.elts;
    for (i = 0; i < prev->cnt_data.nelts; i++) {
        cnt_data = ngx_array_push(&child_data);
        if (cnt_data == NULL) {
            return NGX_CONF_ERROR;
        }
        *cnt_data = prev_cnt_data[i];
        size = cnt_data->rt_vars.nelts;
        if (size > 0) {
            if (ngx_array_init(&cnt_data->rt_vars, cf->pool, size,
                               sizeof(ngx_http_cnt_rt_var_data_t)) != NGX_OK)
            {
                return NGX_CONF_ERROR;
            }
            for (j = 0; j < size; j++) {
                rt_var = ngx_array_push(&cnt_data->rt_vars);
                if (rt_var == NULL) {
                    return NGX_CONF_ERROR;
                }
                *rt_var = ((ngx_http_cnt_rt_var_data_t *)
                           prev_cnt_data[i].rt_vars.elts)[j];
            }
        }
    }

    cnt_data = conf->cnt_data.elts;
    for (i = 0; i < conf->cnt_data.nelts; i++) {
        if (ngx_http_cnt_merge(cf, &child_data, &cnt_data[i]) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }
    }
    conf->cnt_data = child_data;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_cnt_init_module(ngx_cycle_t *cycle)
{
    ngx_uint_t                            i, j, k;
    ngx_http_core_main_conf_t            *cmcf;
    ngx_http_cnt_main_conf_t             *mcf;
    ngx_http_variable_t                  *cmvars;
    ngx_http_cnt_set_histogram_data_t    *histograms;
    ngx_http_cnt_set_t                   *cnt_sets;
    ngx_http_cnt_var_handle_t            *vars;
    ngx_http_cnt_map_range_index_data_t  *v_data;
    ngx_http_cnt_range_boundary_data_t   *boundary = NULL;

    cmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module);
    cmvars = cmcf->variables.elts;

    mcf = ngx_http_cycle_get_module_main_conf(cycle,
                                              ngx_http_custom_counters_module);

    cnt_sets = mcf->cnt_sets.elts;

    for (i = 0; i < mcf->cnt_sets.nelts; i++) {
        histograms = cnt_sets[i].histograms.elts;
        for (j = 0; j < cnt_sets[i].histograms.nelts; j++) {
            if (cmvars[histograms[j].bound_idx].get_handler
                != ngx_http_cnt_get_range_index)
            {
                continue;
            }
            v_data = (ngx_http_cnt_map_range_index_data_t *)
                    cmvars[histograms[j].bound_idx].data;
            if (v_data->range != NULL) {
                boundary = v_data->range->elts;
            }
            vars = histograms[j].cnt_data.elts;
            for (k = 0; k < histograms[j].cnt_data.nelts; k++) {
                if (boundary == NULL || v_data->range == NULL
                    || k > v_data->range->nelts)
                {
                    ngx_str_set(&vars[k].tag, "+Inf");
                    break;
                }
                vars[k].tag = boundary[k].s_value;
            }
        }
    }

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    if (ngx_http_cnt_init_persistent_storage(cycle) != NGX_OK) {
        return NGX_ERROR;
    }
#endif

    return NGX_OK;
}


static void
ngx_http_cnt_exit_master(ngx_cycle_t *cycle)
{
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    (void) ngx_http_cnt_write_persistent_counters(NULL, cycle, 0);
#endif
}


static ngx_int_t
ngx_http_cnt_shm_init(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_atomic_int_t         *shm_data, *oshm_data = data;
    ngx_http_cnt_shm_data_t  *bound_shm_data = shm_zone->data;

    ngx_slab_pool_t          *shpool;
    ngx_http_cnt_set_t       *cnt_sets, *cnt_set;
    ngx_int_t                 nelts;
    size_t                    size;

    cnt_sets = bound_shm_data->cnt_sets->elts;
    cnt_set = &cnt_sets[bound_shm_data->cnt_set];

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    nelts = cnt_set->vars.nelts;
    size = sizeof(ngx_atomic_int_t) * (nelts + 1);

    if (oshm_data != NULL) {
        if (cnt_set->survive_reload) {
            if (nelts == oshm_data[0]) {
                shm_zone->data = data;
                return NGX_OK;
            } else {
                ngx_log_error(NGX_LOG_WARN, shm_zone->shm.log, 0,
                              "custom counters set \"%V\" cannot survive "
                              "reload because its size has changed",
                              &cnt_set->name);
            }
        } else if (nelts <= oshm_data[0]) {
            ngx_shmtx_lock(&shpool->mutex);
            ngx_memzero(oshm_data, size);
            oshm_data[0] = nelts;
            ngx_shmtx_unlock(&shpool->mutex);
            shm_zone->data = oshm_data;
            return NGX_OK;
        }
    }

    if (shm_zone->shm.exists) {
        shm_zone->data = shpool->data;
        return NGX_OK;
    }

    ngx_shmtx_lock(&shpool->mutex);

    shm_data = ngx_slab_calloc_locked(shpool, size);
    if (shm_data == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_ERROR;
    }
    shm_data[0] = nelts;

    if (oshm_data == NULL) {
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
        if (ngx_http_cnt_load_persistent_counters(shm_zone->shm.log,
                                    bound_shm_data->persistent_collection,
                                    bound_shm_data->persistent_collection_tok,
                                    bound_shm_data->persistent_collection_size,
                                    cnt_set->name, &cnt_set->vars,
                                    shm_data + 1) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_ERR, shm_zone->shm.log, 0,
                          "failed to load persistent counters collection, "
                          "proceeding anyway");
        }
#endif
    } else {
        /* FIXME: this is not always safe: too slow workers may write in
         * recently allocated areas when nginx reloads its configuration too
         * fast and having been already freed areas get reused */
        ngx_slab_free_locked(shpool, oshm_data);
    }

    ngx_shmtx_unlock(&shpool->mutex);

    shpool->data = shm_data;
    shm_zone->data = shm_data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_get_value(ngx_http_request_t *r, ngx_http_variable_value_t *v,
                       uintptr_t  data)
{
    ngx_array_t                       *v_data = (ngx_array_t *) data;

    ngx_uint_t                         i;
    ngx_http_cnt_main_conf_t          *mcf;
    ngx_http_cnt_srv_conf_t           *scf;
    ngx_shm_zone_t                    *shm;
    ngx_http_cnt_var_data_t           *var_data;
    volatile ngx_atomic_int_t         *shm_data;
    ngx_http_cnt_set_t                *cnt_sets, *cnt_set;
    ngx_int_t                          idx = NGX_ERROR;
    u_char                            *buf, *last;

    if (v_data == NULL) {
        return NGX_ERROR;
    }
    var_data = v_data->elts;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        goto unreachable_cnt;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);
    cnt_sets = mcf->cnt_sets.elts;
    cnt_set = &cnt_sets[scf->cnt_set];

    shm = cnt_set->zone;
    if (shm == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < v_data->nelts; i++) {
        if (var_data[i].cnt_set != scf->cnt_set) {
            continue;
        }

        idx = var_data[i].self;
        break;
    }
    if (idx == NGX_ERROR) {
        goto unreachable_cnt;
    }

    shm_data = (volatile ngx_atomic_int_t *) shm->data + 1;
    buf = ngx_pnalloc(r->pool, NGX_ATOMIC_T_LEN);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    last = ngx_sprintf(buf, "%A", shm_data[idx]);

    v->len          = last - buf;
    v->data         = buf;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;

unreachable_cnt:

    v->len          = scf->unreachable_cnt_mark.len;
    v->data         = scf->unreachable_cnt_mark.data;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_collection(ngx_http_request_t *r, ngx_http_variable_value_t *v,
                        uintptr_t data)
{
    ngx_str_t  collection;

    if (ngx_http_cnt_build_collection(r, NULL, &collection, 0) != NGX_OK) {
        return NGX_ERROR;
    }

    v->len          = collection.len;
    v->data         = collection.data;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;
}


ngx_int_t
ngx_http_cnt_build_collection(ngx_http_request_t *r, ngx_cycle_t *cycle,
                              ngx_str_t *collection,
                              ngx_uint_t survive_reload_only)
{
    ngx_uint_t                         i, j;
    ngx_http_cnt_main_conf_t          *mcf;
    ngx_pool_t                        *pool;
    ngx_shm_zone_t                    *shm;
    volatile ngx_atomic_int_t         *shm_data;
    ngx_http_cnt_set_t                *cnt_sets;
    ngx_http_cnt_set_var_data_t       *vars;
    ngx_uint_t                         nelts, size;
    ngx_uint_t                         n_cnt_sets = 0;
    u_char                            *buf, *last;

    collection->data = (u_char *) "{}";
    collection->len = 2;

    if (r == NULL) {
        if (cycle == NULL) {
            return NGX_ERROR;
        }
        mcf = ngx_http_cycle_get_module_main_conf(cycle,
                                            ngx_http_custom_counters_module);
        pool = cycle->pool;
    } else {
        mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);
        pool = r->pool;
    }

    nelts = mcf->cnt_sets.nelts;
    if (nelts == 0) {
        return NGX_OK;
    }

    size = mcf->collection_buf_len;
    if (size < 3) {
        return NGX_ERROR;
    }

    buf = ngx_pnalloc(pool, size);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    last = ngx_sprintf(buf, "{");

    cnt_sets = mcf->cnt_sets.elts;
    for (i = 0; i < nelts; i++) {
        if (survive_reload_only && !cnt_sets[i].survive_reload) {
            continue;
        }

        n_cnt_sets++;
        last = ngx_sprintf(last, "\"%V\":{", &cnt_sets[i].name);
        shm = cnt_sets[i].zone;
        shm_data = (volatile ngx_atomic_int_t *) shm->data + 1;

        vars = cnt_sets[i].vars.elts;
        for (j = 0; j < cnt_sets[i].vars.nelts; j++) {
            last = ngx_sprintf(last, "\"%V\":%A,", &vars[j].name,
                               shm_data[vars[j].idx]);
        }
        if (j > 0) {
            last--;
        }

        last = ngx_sprintf(last, "},");
    }
    if (n_cnt_sets > 0) {
        last--;
    }

    last = ngx_sprintf(last, "}");

    collection->data = buf;
    collection->len = last - buf;

    return NGX_OK;
}


static void
ngx_http_cnt_set_collection_buf_len(ngx_http_cnt_main_conf_t *mcf)
{
    ngx_uint_t                         i, j;
    ngx_http_cnt_set_t                *cnt_sets;
    ngx_http_cnt_set_var_data_t       *vars;
    ngx_uint_t                         len = 2;

    cnt_sets = mcf->cnt_sets.elts;
    for (i = 0; i < mcf->cnt_sets.nelts; i++) {
        len += 2 + 2 + 1 + 1 + cnt_sets[i].name.len;

        vars = cnt_sets[i].vars.elts;
        for (j = 0; j < cnt_sets[i].vars.nelts; j++) {
            len += 2 + 1 + 1 + vars[j].name.len + NGX_ATOMIC_T_LEN;
        }
    }

    mcf->collection_buf_len = len;
}


static ngx_int_t
ngx_http_cnt_uptime(ngx_http_request_t *r, ngx_http_variable_value_t *v,
                    uintptr_t data)
{
    u_char                            *buf, *last;
    time_t                             now, start;

    buf = ngx_pnalloc(r->pool, NGX_TIME_T_LEN);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    now = ngx_time();
    start = data == 0 ? ngx_http_cnt_start_time :
            ngx_http_cnt_start_time_reload;

    last = ngx_sprintf(buf, "%T", now - start);

    v->len          = last - buf;
    v->data         = buf;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_get_range_index(ngx_http_request_t *r,
                             ngx_http_variable_value_t *v, uintptr_t  data)
{
    ngx_http_cnt_map_range_index_data_t  *v_data =
            (ngx_http_cnt_map_range_index_data_t *) data;

    ngx_uint_t                            i;
    ngx_http_variable_value_t            *var;
    static const size_t                   buf_size = 32;
    u_char                                buf[buf_size], *p, *vbuf;
    ngx_int_t                             len;
    ngx_http_cnt_range_boundary_data_t   *range;
    double                                val;

    if (v_data == NULL) {
        goto bad_data;
    }

    var = ngx_http_get_indexed_variable(r, v_data->idx);
    if (var == NULL || !var->valid || var->not_found) {
        goto bad_data;
    }

    len = ngx_min(var->len, buf_size - 1);
    ngx_memcpy(buf, var->data, len);
    buf[len] = '\0';

    errno = 0;
    val = strtod((char *) buf, (char **) &p);
    if (errno != 0 || p - buf < len) {
        goto bad_data;
    }

    if (v_data->range == NULL) {
        v->len          = 1;
        v->data         = (u_char *) "0";
        v->valid        = 1;
        v->no_cacheable = 0;
        v->not_found    = 0;

        return NGX_OK;
    }

    range = v_data->range->elts;

    for (i = 0; i < v_data->range->nelts; i++) {
        if (val <= range[i].value) {
            break;
        }
    }

    vbuf = ngx_pnalloc(r->pool, NGX_INT64_LEN);
    if (vbuf == NULL) {
        return NGX_ERROR;
    }

    p = ngx_sprintf(vbuf, "%i", (ngx_int_t) i);

    v->len          = p - vbuf;
    v->data         = vbuf;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;

bad_data:

    v->len          = 5;
    v->data         = (u_char *) "error";
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_ERROR;
}


ngx_int_t
ngx_http_cnt_counter_set_init(ngx_conf_t *cf, ngx_http_cnt_main_conf_t *mcf,
                              ngx_http_cnt_srv_conf_t *scf)
{
    ngx_uint_t                     i;
    ngx_http_core_srv_conf_t      *cscf;
    ngx_str_t                      cnt_set_id;
    ngx_http_cnt_set_t            *cnt_sets, *cnt_set;
    ngx_str_t                      cnt_name;
    ngx_http_cnt_shm_data_t       *shm_data;

    cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);

    cnt_set_id = scf->cnt_set_id;

    if (cnt_set_id.len == 0) {
        if (cscf->server_names.nelts == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "custom counters require directive "
                               "\"counter_set_id\" or server name");
            return NGX_ERROR;
        }
        cnt_set_id = ((ngx_http_server_name_t *) cscf->server_names.elts)[
                            cscf->server_names.nelts - 1].name;
        if (cnt_set_id.len == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "custom counters require directive "
                               "\"counter_set_id\" or non-empty last server "
                               "name");
            return NGX_ERROR;
        }
    }

    cnt_sets = mcf->cnt_sets.elts;
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        for (i = 0; i < mcf->cnt_sets.nelts; i++) {
            if (cnt_sets[i].name.len == cnt_set_id.len
                && ngx_strncmp(cnt_sets[i].name.data, cnt_set_id.data,
                               cnt_set_id.len) == 0)
            {
                scf->cnt_set = i;
            }
        }
    }
    if (scf->cnt_set != NGX_CONF_UNSET_UINT) {
        return NGX_OK;
    }

    cnt_set = ngx_array_push(&mcf->cnt_sets);
    if (cnt_set == NULL) {
        return NGX_ERROR;
    }
    cnt_set->name = cnt_set_id;
    cnt_name.len = ngx_http_cnt_shm_name_prefix.len + cnt_set_id.len;
    cnt_name.data = ngx_pnalloc(cf->pool, cnt_name.len);
    if (cnt_name.data == NULL) {
        return NGX_ERROR;
    }
    ngx_memcpy(cnt_name.data,
               ngx_http_cnt_shm_name_prefix.data,
               ngx_http_cnt_shm_name_prefix.len);
    ngx_memcpy(cnt_name.data + ngx_http_cnt_shm_name_prefix.len,
               cnt_set_id.data,
               cnt_set_id.len);
    cnt_set->zone = ngx_shared_memory_add(cf, &cnt_name, 2 * ngx_pagesize,
                                          &ngx_http_custom_counters_module);
    if (cnt_set->zone == NULL) {
        return NGX_ERROR;
    }

    if (ngx_array_init(&cnt_set->vars, cf->pool, 1,
                       sizeof(ngx_http_cnt_set_var_data_t)) != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_memzero(&cnt_set->histograms, sizeof(ngx_array_t));

    shm_data = ngx_palloc(cf->pool, sizeof(ngx_http_cnt_shm_data_t));
    if (shm_data == NULL) {
        return NGX_ERROR;
    }

    scf->cnt_set = mcf->cnt_sets.nelts - 1;
    shm_data->cnt_sets = &mcf->cnt_sets;
    shm_data->cnt_set = scf->cnt_set;
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    shm_data->persistent_collection = mcf->persistent_collection;
    shm_data->persistent_collection_tok = mcf->persistent_collection_tok;
    shm_data->persistent_collection_size = mcf->persistent_collection_size;
#endif

    cnt_set->zone->init = ngx_http_cnt_shm_init;
    cnt_set->zone->data = shm_data;
    cnt_set->survive_reload = 0;

    return NGX_OK;
}


ngx_int_t
ngx_http_cnt_var_data_init(ngx_conf_t *cf, ngx_http_cnt_srv_conf_t *scf,
                           ngx_http_variable_t *v, ngx_int_t idx,
                           ngx_http_get_variable_pt handler, ngx_int_t bin_idx)
{
    ngx_uint_t                     i;
    ngx_array_t                   *v_data;
    ngx_http_cnt_var_data_t       *var_data;
    ngx_uint_t                     found = 0;


    if ((ngx_array_t *) v->data == NULL) {
        v_data = ngx_palloc(cf->pool, sizeof(ngx_array_t));
        if (v_data == NULL) {
            return NGX_ERROR;
        }
        if (ngx_array_init(v_data, cf->pool, 1,
                           sizeof(ngx_http_cnt_var_data_t)) != NGX_OK)
        {
            return NGX_ERROR;
        }
        var_data = ngx_array_push(v_data);
        if (var_data == NULL) {
            return NGX_ERROR;
        }
        var_data->cnt_set = scf->cnt_set;
        var_data->self = idx;
        var_data->bin_idx = bin_idx;

        if (v->get_handler == NULL) {
            v->get_handler = handler;
        }
        v->data = (uintptr_t) v_data;
    } else {
        v_data = (ngx_array_t *) v->data;
        var_data = v_data->elts;

        for (i = 0; i < v_data->nelts; i++) {
            if (var_data[i].cnt_set == scf->cnt_set) {
                found = 1;
                break;
            }
        }
        if (!found) {
            var_data = ngx_array_push(v_data);
            if (var_data == NULL) {
                return NGX_ERROR;
            }
            var_data->cnt_set = scf->cnt_set;
            var_data->self = idx;
            var_data->bin_idx = bin_idx;
        }
    }

    return NGX_OK;
}


char *
ngx_http_cnt_counter_impl(ngx_conf_t *cf, ngx_command_t *cmd, void *conf,
                          ngx_uint_t early)
{
    ngx_http_cnt_loc_conf_t       *lcf = conf;

    ngx_uint_t                     i;
    ngx_http_cnt_main_conf_t      *mcf;
    ngx_http_cnt_srv_conf_t       *scf;
    ngx_str_t                     *value;
    ngx_http_variable_t           *v;
    ngx_http_cnt_set_t            *cnt_sets, *cnt_set;
    ngx_http_cnt_data_t            cnt_data;
    ngx_http_cnt_set_var_data_t   *vars, *var;
    ngx_http_cnt_rt_var_data_t    *rt_var;
    ngx_int_t                      idx = NGX_ERROR, v_idx;
    ngx_http_cnt_op_e              op = ngx_http_cnt_op_inc;
    ngx_int_t                      val;
    ngx_uint_t                     negative = 0;

    if (lcf->cnt_data.nalloc == 0
        && ngx_array_init(&lcf->cnt_data, cf->pool, 1,
                          sizeof(ngx_http_cnt_data_t)) != NGX_OK)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate memory for custom counters in "
                           "location configuration data");
        return NGX_CONF_ERROR;
    }

    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_custom_counters_module);
    scf = ngx_http_conf_get_module_srv_conf(cf,
                                            ngx_http_custom_counters_module);

    value = cf->args->elts;

    if (value[1].len < 2 || value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }
    value[1].len--;
    value[1].data++;

    if (ngx_http_cnt_counter_set_init(cf, mcf, scf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }
    v_idx = ngx_http_get_variable_index(cf, &value[1]);
    if (v_idx == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    cnt_sets = mcf->cnt_sets.elts;
    cnt_set = &cnt_sets[scf->cnt_set];
    vars = cnt_set->vars.elts;
    for (i = 0; i < cnt_set->vars.nelts; i++) {
        if (vars[i].self == v_idx) {
            idx = i;
            break;
        }
    }
    if (idx == NGX_ERROR) {
        var = ngx_array_push(&cnt_set->vars);
        if (var == NULL) {
            return NGX_CONF_ERROR;
        }
        idx = i;
        var->self = v_idx;
        var->idx = idx;
        var->name = value[1];
    }
    if (v->get_handler != NULL && v->get_handler != ngx_http_cnt_get_value) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "custom counter variable has a different setter");
        return NGX_CONF_ERROR;
    }
    if (ngx_http_cnt_var_data_init(cf, scf, v, idx, ngx_http_cnt_get_value,
                                   NGX_ERROR)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    val = cf->args->nelts == 2 ? 0 : 1;

    ngx_memzero(&cnt_data.rt_vars, sizeof(ngx_array_t));

    if (cf->args->nelts == 4) {
        if (value[3].len > 1 && value[3].data[0] == '-') {
            value[3].len--;
            value[3].data++;
            negative = 1;
        }
        if (value[3].len > 1 && value[3].data[0] == '$') {
            value[3].len--;
            value[3].data++;
            val = ngx_http_get_variable_index(cf, &value[3]);
            if (val == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }
            if (ngx_array_init(&cnt_data.rt_vars, cf->pool, 1,
                               sizeof(ngx_http_cnt_rt_var_data_t)) != NGX_OK)
            {
                return NGX_CONF_ERROR;
            }
            rt_var = ngx_array_push(&cnt_data.rt_vars);
            if (rt_var == NULL) {
                return NGX_CONF_ERROR;
            }
            rt_var->self = val;
            rt_var->negative = negative;
            /* FIXME: rt_var can be freed later in ngx_http_cnt_merge() after
             * pushing its data into the corresponding lcf->cnt_data storage
             * if the latter was already containing references to run-time
             * variables, but it makes little sense because it's not possible
             * in practice to write a configuration file with a scenario that
             * would lead to huge memory losses */
            val = 0;
        } else {
            val = ngx_atoi(value[3].data, value[3].len);
            if (val == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "not a number \"%V\"", &value[3]);
                return NGX_CONF_ERROR;
            }
            if (negative) {
                val = -val;
            }
        }
    }

    if (cf->args->nelts > 2) {
        if (value[2].len == 3 && ngx_strncmp(value[2].data, "set", 3) == 0) {
            op = ngx_http_cnt_op_set;
        } else if (value[2].len == 4
                   && ngx_strncmp(value[2].data, "undo", 4) == 0)
        {
            if (cf->args->nelts > 3) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "counter operation \"undo\" does not accept arguments");
                return NGX_CONF_ERROR;
            }
            op = ngx_http_cnt_op_undo;
            val = 0;
        } else if (value[2].len != 3
                   || ngx_strncmp(value[2].data, "inc", 3) != 0)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "unknown counter operation \"%V\"", &value[2]);
            return NGX_CONF_ERROR;
        }
    }

    cnt_data.self  = v_idx;
    cnt_data.idx   = idx;
    cnt_data.op    = op;
    cnt_data.value = val;
    cnt_data.early = early;

    return ngx_http_cnt_merge(cf, &lcf->cnt_data, &cnt_data);
}


static char *
ngx_http_cnt_counter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    return ngx_http_cnt_counter_impl(cf, cmd, conf, 0);
}


static char *
ngx_http_cnt_early_counter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    return ngx_http_cnt_counter_impl(cf, cmd, conf, 1);
}


static char *
ngx_http_cnt_counter_set_id(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cnt_srv_conf_t       *scf = conf;
    ngx_str_t                     *value = cf->args->elts;

    if (scf->cnt_set != NGX_CONF_UNSET_UINT) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "directive \"counter_set_id\" must precede "
                           "all server's custom counters declarations");
        return NGX_CONF_ERROR;
    }
    if (scf->cnt_set_id.len > 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate directive \"counter_set_id\"");
        return NGX_CONF_ERROR;
    }

    scf->cnt_set_id = value[1];

    return NGX_CONF_OK;
}


static char *
ngx_http_cnt_map_range_index(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                            i;
    ngx_str_t                            *value;
    ngx_http_variable_t                  *v;
    ngx_int_t                             v_idx;
    ngx_http_cnt_map_range_index_data_t  *v_data;
    ngx_array_t                          *v_range;
    static const size_t                   buf_size = 32;
    u_char                                buf[buf_size], *p;
    ngx_int_t                             len;
    ngx_http_cnt_range_boundary_data_t   *pcur;
    double                                cur, prev = 0.0;

    value = cf->args->elts;

    if (value[2].len < 2 || value[2].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }
    value[2].len--;
    value[2].data++;

    v = ngx_http_add_variable(cf, &value[2], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    v->get_handler = ngx_http_cnt_get_range_index;
    v_data = ngx_pcalloc(cf->pool,
                         sizeof(ngx_http_cnt_map_range_index_data_t));
    if (v_data == NULL) {
        return NGX_CONF_ERROR;
    }
    v->data = (uintptr_t) v_data;

    v_idx = ngx_http_get_variable_index(cf, &value[2]);
    if (v_idx == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    if (value[1].len < 2 || value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }
    value[1].len--;
    value[1].data++;

    v_idx = ngx_http_get_variable_index(cf, &value[1]);
    if (v_idx == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    v_data->idx = v_idx;

    if (cf->args->nelts > 3) {
        v_range = ngx_pcalloc(cf->pool, sizeof(ngx_array_t));
        if (v_range == NULL
            || ngx_array_init(v_range, cf->pool, cf->args->nelts - 3,
                              sizeof(ngx_http_cnt_range_boundary_data_t))
               != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }

        for (i = 3; i < cf->args->nelts; i++) {
            len = ngx_min(value[i].len, buf_size - 1);
            ngx_memcpy(buf, value[i].data, len);
            buf[len] = '\0';

            errno = 0;
            cur = strtod((char *) buf, (char **) &p);
            if (errno != 0 || p - buf < len) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "failed to read \"%V\" as double value",
                                   &value[i]);
                return NGX_CONF_ERROR;
            }

            if (i > 2 && prev >= cur) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "range must increase monotonically");
                return NGX_CONF_ERROR;
            }

            pcur = ngx_array_push(v_range);
            if (pcur == NULL) {
                return NGX_CONF_ERROR;
            }
            pcur->value = cur;
            pcur->s_value = value[i];

            prev = cur;
        }

        v_data->range = v_range;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_cnt_merge(ngx_conf_t *cf, ngx_array_t *dst,
                   ngx_http_cnt_data_t *cnt_data)
{
    ngx_http_cnt_data_t               *data = dst->elts;

    ngx_uint_t                         i, size;
    ngx_http_cnt_data_t               *new_data;
    ngx_http_cnt_rt_var_data_t        *rt_var;
    ngx_int_t                          idx = NGX_ERROR;

    for (i = 0; i < dst->nelts; i++) {
        if (data[i].self == cnt_data->self) {
            idx = i;
            break;
        }
    }
    if (idx == NGX_ERROR) {
        new_data = ngx_array_push(dst);
        if (new_data == NULL) {
            return NGX_CONF_ERROR;
        }
        *new_data = *cnt_data;
    } else {
        new_data = &data[idx];
        if (new_data->early != cnt_data->early) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "custom counter was set both normal and early "
                               "in the same scope");
            return NGX_CONF_ERROR;
        }
        if (cnt_data->op == ngx_http_cnt_op_inc) {
            if (new_data->op == ngx_http_cnt_op_undo) {
                new_data->op = ngx_http_cnt_op_inc;
            }
            new_data->value += cnt_data->value;
            size = cnt_data->rt_vars.nelts;
            if (size > 0) {
                if (new_data->rt_vars.nalloc == 0
                    && ngx_array_init(&new_data->rt_vars, cf->pool, size,
                                      sizeof(ngx_http_cnt_rt_var_data_t))
                       != NGX_OK)
                {
                    return NGX_CONF_ERROR;
                }
                for (i = 0; i < size; i++) {
                    rt_var = ngx_array_push(&new_data->rt_vars);
                    if (rt_var == NULL) {
                        return NGX_CONF_ERROR;
                    }
                    *rt_var = ((ngx_http_cnt_rt_var_data_t *)
                               cnt_data->rt_vars.elts)[i];
                }
            }
        } else {
            *new_data = *cnt_data;
        }
    }

    return NGX_CONF_OK;
}


static ngx_inline ngx_int_t
ngx_http_cnt_phase_handler_impl(ngx_http_request_t *r, ngx_uint_t early)
{
    if (r != r->main) {
        return NGX_DECLINED;
    }

    (void) ngx_http_cnt_update(r, early);

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_cnt_rewrite_phase_handler(ngx_http_request_t *r)
{
    return ngx_http_cnt_phase_handler_impl(r, 1);
}


static ngx_int_t
ngx_http_cnt_log_phase_handler(ngx_http_request_t *r)
{
    return ngx_http_cnt_phase_handler_impl(r, 0);
}


static ngx_int_t
ngx_http_cnt_update(ngx_http_request_t *r, ngx_uint_t early)
{
    ngx_uint_t                     i, j;
    ngx_http_cnt_main_conf_t      *mcf;
    ngx_http_cnt_srv_conf_t       *scf;
    ngx_http_cnt_loc_conf_t       *lcf;
    ngx_http_core_main_conf_t     *cmcf;
    ngx_http_cnt_data_t           *cnt_data;
    volatile ngx_atomic_int_t     *shm_data, *dst;
    ngx_slab_pool_t               *shpool;
    ngx_http_cnt_rt_var_data_t    *rt_vars;
    ngx_http_variable_value_t     *var;
    ngx_http_cnt_set_t            *cnt_sets, *cnt_set;
    ngx_int_t                      value, val;
    ngx_str_t                      base_var;
    ngx_http_variable_t           *v;
    ngx_uint_t                     negative, invalid;
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    time_t                         now;
#endif

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        return NGX_DECLINED;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);

    cnt_sets = mcf->cnt_sets.elts;
    cnt_set = &cnt_sets[scf->cnt_set];

    shm_data = (volatile ngx_atomic_int_t *) cnt_set->zone->data + 1;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_counters_module);
    cnt_data = lcf->cnt_data.elts;

    for (i = 0; i < lcf->cnt_data.nelts; i++) {
        if (cnt_data[i].early != early) {
            continue;
        }
        dst = &shm_data[cnt_data[i].idx];
        rt_vars = cnt_data[i].rt_vars.elts;
        value = cnt_data[i].value;
        invalid = 0;
        for (j = 0; j < cnt_data[i].rt_vars.nelts; j++) {
            var = ngx_http_get_indexed_variable(r, rt_vars[j].self);
            if (var == NULL || !var->valid || var->not_found) {
                invalid = 1;
                continue;
            }
            negative = 0;
            base_var.len = var->len;
            base_var.data = var->data;
            if (var->len > 1 && var->data[0] == '-') {
                base_var.len--;
                base_var.data++;
                negative = 1;
            }
            val = ngx_atoi(base_var.data, base_var.len);
            if (val == NGX_ERROR) {
                cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
                v = cmcf->variables.elts;
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "[custom counters] variable \"%V\" has value "
                              "\"%v\" which is not a number",
                              &v[rt_vars[j].self].name, var);
                invalid = 1;
                continue;
            }
            if (negative) {
                val = -val;
            }
            value += rt_vars[j].negative ? -val : val;
        }
        if (invalid) {
            continue;
        }
        if (cnt_data[i].op == ngx_http_cnt_op_set) {
            shpool = (ngx_slab_pool_t *) cnt_set->zone->shm.addr;
            ngx_shmtx_lock(&shpool->mutex);
            *dst = value;
            ngx_shmtx_unlock(&shpool->mutex);
        } else if (cnt_data[i].op == ngx_http_cnt_op_inc) {
            if (value != 0) {
                /* FIXME: currently there is no protection against overflows
                 * and underflows, e.g. value 9223372036854775807 on a 64-bit
                 * architecture will become -9223372036854775808 rather than 0
                 * after incrementing by one */
                (void) ngx_atomic_fetch_add(dst, value);
            }
        }
    }

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    if (early) {
        return NGX_OK;
    }

    now = ngx_time();

    if (mcf->persistent_collection_check > 0
        && now - mcf->persistent_collection_last_check
            > mcf->persistent_collection_check)
    {
        if (ngx_http_cnt_write_persistent_counters(r, NULL, 1) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "failed to save persistent counters backup");
        }
        mcf->persistent_collection_last_check = now;
    }
#endif

    return NGX_OK;
}

