/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_module.c
 *
 *    Description:  nginx module for shared custom counters
 *
 *        Version:  2.0
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
#include <ngx_core.h>
#include <ngx_http.h>

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
#define STATIC_JSMN
#define JSMN_STRICT
#include <jsmn.h>
#endif


static time_t  ngx_http_cnt_start_time;
static time_t  ngx_http_cnt_start_time_reload;

static const ngx_str_t  ngx_http_cnt_shm_name_prefix =
    ngx_string("custom_counters_");
static const ngx_int_t  ngx_http_cnt_histogram_max_bins = 32;


typedef enum {
    ngx_http_cnt_op_set,
    ngx_http_cnt_op_inc,
    ngx_http_cnt_op_undo
} ngx_http_cnt_op_e;


typedef enum {
    ngx_http_cnt_histogram_sum,
    ngx_http_cnt_histogram_err
} ngx_http_cnt_histogram_special_var_e;


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
    ngx_int_t                   self;
    ngx_int_t                   idx;
    ngx_str_t                   name;
} ngx_http_cnt_set_var_data_t;


typedef struct {
    ngx_int_t                   idx;
    ngx_str_t                   name;
    ngx_str_t                   tag;         /* boundary for histogram bins */
} ngx_http_cnt_var_handle_t;


typedef struct {
    ngx_int_t                   self;
    ngx_int_t                   bound_idx;
    ngx_array_t                 cnt_data;
    ngx_http_cnt_var_handle_t   cnt_sum;
    ngx_http_cnt_var_handle_t   cnt_err;
} ngx_http_cnt_set_histogram_data_t;


typedef struct {
    ngx_int_t                   idx;
    ngx_array_t                *range;
} ngx_http_cnt_map_range_index_data_t;


typedef struct {
    double                      value;
    ngx_str_t                   s_value;
} ngx_http_cnt_range_boundary_data_t;


typedef struct {
    ngx_str_t                   name;
    ngx_array_t                 vars;
    ngx_array_t                 histograms;
    ngx_shm_zone_t             *zone;
    ngx_uint_t                  survive_reload;
} ngx_http_cnt_set_t;


typedef struct {
    ngx_uint_t                  cnt_set;
    ngx_int_t                   self;
    ngx_int_t                   bin_idx;
} ngx_http_cnt_var_data_t;


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
    ngx_array_t                 cnt_sets;
    ngx_uint_t                  collection_buf_len;
#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
    ngx_str_t                   persistent_storage;
    ngx_str_t                   persistent_storage_backup;
    ngx_str_t                   persistent_collection;
    jsmntok_t                  *persistent_collection_tok;
    int                         persistent_collection_size;
    time_t                      persistent_collection_check;
    time_t                      persistent_collection_last_check;
    ngx_uint_t                  persistent_storage_backup_requires_init;
#endif
} ngx_http_cnt_main_conf_t;


typedef struct {
    ngx_uint_t                  cnt_set;
    ngx_str_t                   cnt_set_id;
    ngx_str_t                   unreachable_cnt_mark;
    ngx_flag_t                  survive_reload;
} ngx_http_cnt_srv_conf_t;


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
static ngx_int_t ngx_http_cnt_build_collection(ngx_http_request_t *r,
    ngx_cycle_t *cycle, ngx_str_t *collection, ngx_uint_t survive_reload_only);
static void ngx_http_cnt_set_collection_buf_len(ngx_http_cnt_main_conf_t *mcf);
static ngx_int_t ngx_http_cnt_uptime(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_cnt_get_histogram_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static ngx_int_t ngx_http_cnt_get_histogram_inc_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static ngx_int_t ngx_http_cnt_get_range_index(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static ngx_int_t ngx_http_cnt_counter_set_init(ngx_conf_t *cf,
    ngx_http_cnt_main_conf_t *mcf, ngx_http_cnt_srv_conf_t *scf);
static ngx_int_t ngx_http_cnt_var_data_init(ngx_conf_t *cf,
    ngx_http_cnt_srv_conf_t *scf, ngx_http_variable_t *v, ngx_int_t idx,
    ngx_http_get_variable_pt handler, ngx_int_t bin_idx);
static char *ngx_http_cnt_counter_impl(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf, ngx_uint_t early);
static char *ngx_http_cnt_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_early_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_counter_set_id(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_cnt_histogram_special_var(ngx_conf_t *cf, void *conf,
    ngx_http_cnt_srv_conf_t *scf, ngx_str_t *counter_name,
    ngx_str_t *counter_op_value, ngx_str_t base_name, ngx_int_t idx,
    ngx_http_cnt_set_histogram_data_t *data,
    ngx_http_cnt_histogram_special_var_e type);
static char *ngx_http_cnt_histogram(ngx_conf_t *cf, ngx_command_t *cmd,
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

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
static char *ngx_http_cnt_counters_persistent_storage(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_cnt_init_persistent_storage(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cnt_load_persistent_counters(ngx_log_t* log,
    ngx_str_t collection, jsmntok_t *collection_tok, int collection_size,
    ngx_str_t cnt_set, ngx_array_t *vars, ngx_atomic_int_t *shm_data);
static ngx_int_t ngx_http_cnt_write_persistent_counters(ngx_http_request_t *r,
    ngx_cycle_t *cycle, ngx_uint_t backup);
#endif


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


static ngx_int_t
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
ngx_http_cnt_get_histogram_value(ngx_http_request_t *r,
                                 ngx_http_variable_value_t *v, uintptr_t  data)
{
    ngx_array_t                        *v_data = (ngx_array_t *) data;

    ngx_uint_t                          i;
    ngx_http_cnt_main_conf_t           *mcf;
    ngx_http_cnt_srv_conf_t            *scf;
    ngx_http_cnt_var_data_t            *var_data;
    ngx_http_cnt_set_t                 *cnt_sets, *cnt_set;
    ngx_http_cnt_var_handle_t          *cnt_data;
    ngx_array_t                         cnt_values;
    ngx_str_t                          *values, *value = NULL;
    ngx_http_variable_value_t          *var;
    ngx_http_cnt_set_histogram_data_t  *histograms;
    ngx_uint_t                          written = 0;
    u_char                             *buf;
    ngx_int_t                           len = 0;
    ngx_int_t                           idx = NGX_ERROR;

    if (v_data == NULL) {
        return NGX_ERROR;
    }
    var_data = v_data->elts;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        goto unreachable_histogram;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);
    cnt_sets = mcf->cnt_sets.elts;
    cnt_set = &cnt_sets[scf->cnt_set];

    for (i = 0; i < v_data->nelts; i++) {
        if (var_data[i].cnt_set != scf->cnt_set) {
            continue;
        }

        idx = var_data[i].self;
        break;
    }
    if (idx == NGX_ERROR) {
        goto unreachable_histogram;
    }

    histograms = cnt_set->histograms.elts;

    cnt_data = histograms[idx].cnt_data.elts;
    if (ngx_array_init(&cnt_values, r->pool, histograms[idx].cnt_data.nelts,
                       sizeof(ngx_str_t)) != NGX_OK)
    {
        return NGX_ERROR;
    }

    for (i = 0; i < histograms[idx].cnt_data.nelts; i++) {
        var = ngx_http_get_indexed_variable(r, cnt_data[i].idx);
        if (var == NULL || !var->valid || var->not_found) {
            return NGX_ERROR;
        }
        value = ngx_array_push(&cnt_values);
        if (value == NULL) {
            return NGX_ERROR;
        }
        value->len = var->len;
        value->data = var->data;
        len += var->len + 1;
    }

    if (len == 0) {
        v->len          = 0;
        v->data         = NULL;
        v->valid        = 1;
        v->no_cacheable = 0;
        v->not_found    = 0;

        return NGX_OK;
    }

    buf = ngx_pnalloc(r->pool, len);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    values = cnt_values.elts;
    for (i = 0; i < cnt_values.nelts; i++) {
        ngx_memcpy(buf + written, values[i].data, values[i].len);
        written += values[i].len;
        ngx_memcpy(buf + written, ",", 1);
        ++written;
    }

    v->len          = --len;
    v->data         = buf;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;

unreachable_histogram:

    v->len          = 0;
    v->data         = NULL;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_get_histogram_inc_value(ngx_http_request_t *r,
                                     ngx_http_variable_value_t *v,
                                     uintptr_t  data)
{
    ngx_array_t                        *v_data = (ngx_array_t *) data;

    ngx_uint_t                          i;
    ngx_http_cnt_main_conf_t           *mcf;
    ngx_http_cnt_srv_conf_t            *scf;
    ngx_http_cnt_var_data_t            *var_data;
    ngx_http_cnt_set_t                 *cnt_sets, *cnt_set;
    ngx_int_t                           bound_idx;
    ngx_http_variable_value_t          *var;
    ngx_http_cnt_set_histogram_data_t  *histograms;
    ngx_int_t                           val, bin_idx;
    ngx_int_t                           idx = NGX_ERROR;
    ngx_uint_t                          inc = 0;

    if (v_data == NULL) {
        return NGX_ERROR;
    }
    var_data = v_data->elts;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        goto unreachable_histogram;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);
    cnt_sets = mcf->cnt_sets.elts;
    cnt_set = &cnt_sets[scf->cnt_set];

    for (i = 0; i < v_data->nelts; i++) {
        if (var_data[i].cnt_set != scf->cnt_set) {
            continue;
        }

        idx = var_data[i].self;
        break;
    }
    if (idx == NGX_ERROR) {
        goto unreachable_histogram;
    }

    histograms = cnt_set->histograms.elts;
    bin_idx = var_data[i].bin_idx;

    bound_idx = histograms[idx].bound_idx;
    if (bound_idx < 0) {
        return NGX_ERROR;
    }

    var = ngx_http_get_indexed_variable(r, bound_idx);

    if (var == NULL || !var->valid || var->not_found) {
        if (bin_idx == NGX_ERROR) {
            inc = 1;
        }
    } else {
        val = ngx_atoi(var->data, var->len);

        if (val == NGX_ERROR) {
            if (bin_idx == NGX_ERROR) {
                inc = 1;
            }
        } else {
            if (val >= (ngx_int_t) histograms[idx].cnt_data.nelts) {
                if (bin_idx == NGX_ERROR) {
                    inc = 1;
                }
            } else if (val == bin_idx || bin_idx == NGX_DONE) {
                inc = 1;
            }
        }
    }

    v->len          = 1;
    v->data         = inc ? (u_char *) "1" : (u_char *) "0";
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

    return NGX_OK;

unreachable_histogram:

    v->len          = 1;
    v->data         = (u_char *) "0";
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


static ngx_int_t
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


static ngx_int_t
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


static char *
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


static ngx_int_t
ngx_http_cnt_histogram_special_var(ngx_conf_t *cf, void *conf,
                                   ngx_http_cnt_srv_conf_t *scf,
                                   ngx_str_t *counter_name,
                                   ngx_str_t *counter_op_value,
                                   ngx_str_t base_name, ngx_int_t idx,
                                   ngx_http_cnt_set_histogram_data_t *data,
                                   ngx_http_cnt_histogram_special_var_e type)
{
    ngx_str_t                           inc_var_name;
    ngx_http_variable_t                *v;
    ngx_int_t                           v_idx;
    const char                         *part = NULL;
    ngx_int_t                           bin_idx = NGX_ERROR;
    size_t                              size = 0;

    switch (type) {
    case ngx_http_cnt_histogram_sum:
        part = "_sum";
        size = 4;
        bin_idx = NGX_DONE;
        break;
    case ngx_http_cnt_histogram_err:
        part = "_err";
        size = 4;
        bin_idx = NGX_ERROR;
        break;
    default:
        break;
    }

    counter_name->len = base_name.len + 5;
    counter_name->data = ngx_pnalloc(cf->pool, counter_name->len);
    if (counter_name->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate memory for histogram data");
        return NGX_ERROR;
    }
    counter_name->data[0] = '$';
    ngx_memcpy(counter_name->data + 1, base_name.data, base_name.len);
    ngx_memcpy(counter_name->data + 1 + base_name.len, part, size);
    counter_name->data += 1;
    counter_name->len -= 1;
    v = ngx_http_add_variable(cf, counter_name, NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_ERROR;
    }
    v_idx = ngx_http_get_variable_index(cf, counter_name);
    if (v_idx == NGX_ERROR) {
        return NGX_ERROR;
    }
    counter_name->data -= 1;
    counter_name->len += 1;
    switch (type) {
    case ngx_http_cnt_histogram_sum:
        data->cnt_sum.idx = v_idx;
        data->cnt_sum.name = *counter_name;
        ngx_str_set(&data->cnt_sum.tag, "sum");
        break;
    case ngx_http_cnt_histogram_err:
        data->cnt_err.idx = v_idx;
        data->cnt_err.name = *counter_name;
        ngx_str_set(&data->cnt_err.tag, "err");
        break;
    default:
        break;
    }
    counter_op_value->len = counter_name->len + 4;
    counter_op_value->data = ngx_pnalloc(cf->pool, counter_op_value->len);
    if (counter_op_value->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate memory for histogram data");
        return NGX_ERROR;
    }
    counter_op_value->data[0] = '$';
    ngx_memcpy(counter_op_value->data + 1, "inc_", 4);
    ngx_memcpy(counter_op_value->data + 1 + 4, counter_name->data + 1,
               counter_name->len - 1);
    if (ngx_http_cnt_counter_impl(cf, NULL, conf, 0)) {
        return NGX_ERROR;
    }
    inc_var_name.len = counter_name->len + 4;
    inc_var_name.data = ngx_pnalloc(cf->pool, inc_var_name.len);
    if (inc_var_name.data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate memory for histogram data");
        return NGX_ERROR;
    }
    ngx_memcpy(inc_var_name.data, "inc_", 4);
    ngx_memcpy(inc_var_name.data + 4, counter_name->data,
               counter_name->len);
    v = ngx_http_add_variable(cf, &inc_var_name,
                              NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOHASH);
    if (v == NULL) {
        return NGX_ERROR;
    }
    if (v->get_handler != NULL
        && v->get_handler
            != ngx_http_cnt_get_histogram_inc_value)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "histogram inc variable has a different setter");
        return NGX_ERROR;
    }
    if (ngx_http_cnt_var_data_init(cf, scf, v, idx,
                                   ngx_http_cnt_get_histogram_inc_value,
                                   bin_idx)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static char *
ngx_http_cnt_histogram(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                          i, last_i;
    ngx_int_t                           j;
    ngx_http_cnt_main_conf_t           *mcf;
    ngx_http_cnt_srv_conf_t            *scf;
    ngx_str_t                          *value;
    ngx_http_variable_t                *v, *cnt_v, *inc_v;
    ngx_http_cnt_set_t                 *cnt_sets, *cnt_set;
    ngx_http_cnt_set_histogram_data_t  *vars, *var;
    ngx_http_cnt_var_handle_t          *cnt_data, *cnt; 
    ngx_int_t                           idx = NGX_ERROR;
    ngx_int_t                           v_idx, cnt_v_idx;
    ngx_int_t                           val;
    ngx_conf_t                          cf_cnt;
    ngx_array_t                         cf_cnt_args;
    ngx_str_t                          *counter_cmd, *counter_name;
    ngx_str_t                          *counter_op, *counter_op_value;
    ngx_str_t                           inc_var_name;

    value = cf->args->elts;

    if (value[1].len < 2 || value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }
    value[1].len--;
    value[1].data++;

    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_custom_counters_module);
    scf = ngx_http_conf_get_module_srv_conf(cf,
                                            ngx_http_custom_counters_module);

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

    if (cnt_set->histograms.nalloc == 0
        && ngx_array_init(&cnt_set->histograms, cf->pool, 1,
                          sizeof(ngx_http_cnt_set_histogram_data_t))
            != NGX_OK)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate memory for histogram data");
        return NGX_CONF_ERROR;
    }

    vars = cnt_set->histograms.elts;
    for (i = 0; i < cnt_set->histograms.nelts; i++) {
        if (vars[i].self == v_idx) {
            idx = i;
            break;
        }
    }
    last_i = i;

    if (v->get_handler != NULL
        && v->get_handler != ngx_http_cnt_get_histogram_value)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "histogram variable has a different setter");
        return NGX_CONF_ERROR;
    }

    if (ngx_array_init(&cf_cnt_args, cf->pool, 4, sizeof(ngx_str_t)) != NGX_OK)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate memory for histogram data");
        return NGX_CONF_ERROR;
    }

    counter_cmd = ngx_array_push(&cf_cnt_args);
    if (counter_cmd == NULL) {
        return NGX_CONF_ERROR;
    }
    ngx_str_set(counter_cmd, "counter");
    counter_name = ngx_array_push(&cf_cnt_args);
    if (counter_name == NULL) {
        return NGX_CONF_ERROR;
    }

    cf_cnt = *cf;
    cf_cnt.args = &cf_cnt_args;

    if (cf->args->nelts == 4) {
        val = ngx_atoi(value[2].data, value[2].len);
        if (val == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "not a number \"%V\"",
                               &value[2]);
            return NGX_CONF_ERROR;
        }
        if (val == 0 || val > ngx_http_cnt_histogram_max_bins) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "number of bins must be greater than 0 but "
                               "not greater than %i",
                               ngx_http_cnt_histogram_max_bins);
            return NGX_CONF_ERROR;
        }
        if (idx != NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "histogram \"%V\" "
                               "was already declared in this counter set",
                               &value[1]);
            return NGX_CONF_ERROR;
        }
        if (value[3].len < 2 || value[3].data[0] != '$') {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid variable name \"%V\"", &value[3]);
            return NGX_CONF_ERROR;
        }
        value[3].len--;
        value[3].data++;

        var = ngx_array_push(&cnt_set->histograms);
        if (var == NULL) {
            return NGX_CONF_ERROR;
        }
        if (ngx_array_init(&var->cnt_data, cf->pool, val,
                           sizeof(ngx_http_cnt_var_handle_t)) != NGX_OK)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "failed to allocate memory for histogram data");
            return NGX_CONF_ERROR;
        }
        idx = last_i;

        var->bound_idx = ngx_http_get_variable_index(cf, &value[3]);
        if (var->bound_idx == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }

        counter_op = ngx_array_push(&cf_cnt_args);
        if (counter_op == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_str_set(counter_op, "inc");
        counter_op_value = ngx_array_push(&cf_cnt_args);
        if (counter_op_value == NULL) {
            return NGX_CONF_ERROR;
        }

        for (j = 0; j < val; j++) {
            counter_name->len = value[1].len + 4;
            counter_name->data = ngx_pnalloc(cf->pool, counter_name->len);
            if (counter_name->data == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "failed to allocate memory for histogram data");
                return NGX_CONF_ERROR;
            }
            counter_name->data[0] = '$';
            ngx_memcpy(counter_name->data + 1, value[1].data, value[1].len);
            (void) ngx_snprintf(counter_name->data + value[1].len + 1, 3,
                                "_%02i", j);
            counter_name->data += 1;
            counter_name->len -= 1;
            cnt_v = ngx_http_add_variable(cf, counter_name,
                                          NGX_HTTP_VAR_CHANGEABLE);
            if (cnt_v == NULL) {
                return NGX_CONF_ERROR;
            }
            cnt_v_idx = ngx_http_get_variable_index(cf, counter_name);
            if (cnt_v_idx == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }
            cnt = ngx_array_push(&var->cnt_data);
            if (cnt == NULL) {
                return NGX_CONF_ERROR;
            }
            counter_name->data -= 1;
            counter_name->len += 1;
            cnt->idx = cnt_v_idx;
            cnt->name = *counter_name;
            ngx_str_null(&cnt->tag);
            counter_op_value->len = counter_name->len + 4;
            counter_op_value->data = ngx_pnalloc(cf->pool,
                                                 counter_op_value->len);
            if (counter_op_value->data == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "failed to allocate memory for histogram data");
                return NGX_CONF_ERROR;
            }
            counter_op_value->data[0] = '$';
            ngx_memcpy(counter_op_value->data + 1, "inc_", 4);
            ngx_memcpy(counter_op_value->data + 1 + 4, counter_name->data + 1,
                       counter_name->len - 1);
            if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                return NGX_CONF_ERROR;
            }
            inc_var_name.len = counter_name->len + 4;
            inc_var_name.data = ngx_pnalloc(cf->pool, inc_var_name.len);
            if (inc_var_name.data == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "failed to allocate memory for histogram data");
                return NGX_CONF_ERROR;
            }
            ngx_memcpy(inc_var_name.data, "inc_", 4);
            ngx_memcpy(inc_var_name.data + 4, counter_name->data,
                       counter_name->len);
            inc_v = ngx_http_add_variable(cf, &inc_var_name,
                                NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOHASH);
            if (inc_v == NULL) {
                return NGX_CONF_ERROR;
            }
            if (inc_v->get_handler != NULL
                && inc_v->get_handler != ngx_http_cnt_get_histogram_inc_value)
            {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "histogram inc variable has a different setter");
                return NGX_CONF_ERROR;
            }
            if (ngx_http_cnt_var_data_init(cf, scf, inc_v, idx,
                                           ngx_http_cnt_get_histogram_inc_value,
                                           j)
                != NGX_OK)
            {
                return NGX_CONF_ERROR;
            }
        }
        if (ngx_http_cnt_histogram_special_var(&cf_cnt, conf, scf, counter_name,
                                               counter_op_value, value[1], idx,
                                               var, ngx_http_cnt_histogram_sum)
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
        if (ngx_http_cnt_histogram_special_var(&cf_cnt, conf, scf, counter_name,
                                               counter_op_value, value[1], idx,
                                               var, ngx_http_cnt_histogram_err)
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }

        var->self = v_idx;
        if (ngx_http_cnt_var_data_init(cf, scf, v, idx,
                                       ngx_http_cnt_get_histogram_value,
                                       NGX_ERROR)
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    } else if (cf->args->nelts == 3) {
        cnt_data = vars[idx].cnt_data.elts;

        if (idx == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "histogram \"%V\" was "
                               "not declared in this counter set", &value[1]);
            return NGX_CONF_ERROR;
        }

        if (value[2].len == 4 && ngx_strncmp(value[2].data, "undo", 4) == 0) {
            counter_op = ngx_array_push(&cf_cnt_args);
            if (counter_op == NULL) {
                return NGX_CONF_ERROR;
            }
            ngx_str_set(counter_op, "undo");
            for (i = 0; i < vars[idx].cnt_data.nelts; i++) {
                *counter_name = cnt_data[i].name;
                if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                    return NGX_CONF_ERROR;
                }
            }
            *counter_name = vars[idx].cnt_sum.name;
            if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                return NGX_CONF_ERROR;
            }
            *counter_name = vars[idx].cnt_err.name;
            if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                return NGX_CONF_ERROR;
            }
        } else if (value[2].len == 5
                   && ngx_strncmp(value[2].data, "reset", 5) == 0)
        {
            counter_op = ngx_array_push(&cf_cnt_args);
            if (counter_op == NULL) {
                return NGX_CONF_ERROR;
            }
            ngx_str_set(counter_op, "set");
            counter_op_value = ngx_array_push(&cf_cnt_args);
            if (counter_op_value == NULL) {
                return NGX_CONF_ERROR;
            }
            ngx_str_set(counter_op_value, "0");
            for (i = 0; i < vars[idx].cnt_data.nelts; i++) {
                *counter_name = cnt_data[i].name;
                if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                    return NGX_CONF_ERROR;
                }
            }
            *counter_name = vars[idx].cnt_sum.name;
            if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                return NGX_CONF_ERROR;
            }
            *counter_name = vars[idx].cnt_err.name;
            if (ngx_http_cnt_counter_impl(&cf_cnt, NULL, conf, 0)) {
                return NGX_CONF_ERROR;
            }
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "unknown histogram operation \"%V\"", &value[2]);
            return NGX_CONF_ERROR;
        }
    }

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


#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY

static char *
ngx_http_cnt_counters_persistent_storage(ngx_conf_t *cf, ngx_command_t *cmd,
                                         void *conf)
{
    ngx_http_cnt_main_conf_t      *mcf = conf;
    ngx_str_t                     *value = cf->args->elts;
    ngx_str_t                      path;
    ngx_file_t                     file, backup_file;
    ngx_file_info_t                file_info, file_info_backup;
    size_t                         file_size = 0;
    ngx_copy_file_t                copy_file;
    ngx_fd_t                       fd;
    u_char                        *buf = NULL;
    ssize_t                        n;
    ngx_uint_t                     len;
    jsmn_parser                    jparse;
    jsmntok_t                     *jtok = NULL;
    int                            jrc, jsz;
    ngx_uint_t                     not_found = 0, backup_not_found = 0;
    ngx_uint_t                     copy_backup_file = 0;
    ngx_uint_t                     copy_backup_started = 0;
    ngx_uint_t                     copy_backup_ok = 0;
    ngx_uint_t                     cleanup_backup = 0;

    if (mcf->persistent_storage.len > 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate directive "
                           "\"counters_persistent_sorage\"");
        return NGX_CONF_ERROR;
    }
    if (mcf->cnt_sets.nelts > 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "directive \"counters_persistent_sorage\" must "
                           "precede all custom counters sets declarations");
        return NGX_CONF_ERROR;
    }

    path = value[1];

    if (path.len == 0 || path.data[path.len - 1] == '/') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "bad file name \"%V\"", &path);
        return NGX_CONF_ERROR;
    }

    if (path.data[0] != '/') {
        if (ngx_get_full_name(cf->pool, &cf->cycle->prefix, &path) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    len = path.len + 1;
    mcf->persistent_storage.data = ngx_pnalloc(cf->pool, len);
    if (mcf->persistent_storage.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(mcf->persistent_storage.data, path.data, path.len);
    mcf->persistent_storage.data[path.len] = '\0';
    mcf->persistent_storage.len = path.len;

    /* BEWARE: unnecessary reading persistent storage on every reload of Nginx,
     * however this should not be very harmful */
    ngx_memzero(&file, sizeof(ngx_file_t));

    file.name = mcf->persistent_storage;
    file.log = cf->log;

    file.fd = ngx_open_file(file.name.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);

    if (file.fd == NGX_INVALID_FILE) {
        ngx_err_t   err = ngx_errno;
        ngx_dir_t   dir;
        ngx_str_t   dir_name;
        u_char     *slash = NULL, *p = file.name.data;

        do {
            p = ngx_strlchr(p, file.name.data + file.name.len, '/');
            if (p != NULL) {
                slash = p++;
            }
        } while (p != NULL);

        if (slash == NULL) {
            dir_name.data = (u_char *) "./";
            dir_name.len = 2;
        } else {
            dir_name.len = slash + 1 - file.name.data;
            dir_name.data = ngx_pnalloc(cf->pool, dir_name.len + 1);
            if (dir_name.data == NULL) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                "failed to allocate memory to parse JSON data");
                return NGX_CONF_ERROR;
            }
            ngx_memcpy(dir_name.data, file.name.data, dir_name.len);
            dir_name.data[dir_name.len] = '\0';
        }

        if (err != ENOENT) {
            ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                               ngx_open_file_n " \"%V\" failed", &file.name);
            return NGX_CONF_ERROR;
        }

        if (ngx_open_dir(&dir_name, &dir) == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                               ngx_open_file_n " \"%V\" failed", &file.name);
            return NGX_CONF_ERROR;
        }

        if (ngx_close_dir(&dir) == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                               ngx_close_dir_n " \"%V\" failed", &dir_name);
        }

        /* the file does not exist yet */
        not_found = 1;
    }

    len = path.len + 2;
    mcf->persistent_storage_backup.data = ngx_pnalloc(cf->pool, len);
    if (mcf->persistent_storage_backup.data == NULL) {
        if (not_found) {
            return NGX_CONF_ERROR;
        } else {
            goto cleanup;
        }
    }

    ngx_memcpy(mcf->persistent_storage_backup.data, path.data, path.len);
    mcf->persistent_storage_backup.data[path.len] = '~';
    mcf->persistent_storage_backup.data[path.len + 1] = '\0';
    mcf->persistent_storage_backup.len = path.len + 1;

    if (ngx_file_info(mcf->persistent_storage_backup.data, &file_info_backup)
        == NGX_FILE_ERROR)
    {
        if (ngx_errno != ENOENT) {
            ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                               ngx_file_info_n " \"%V\" failed",
                               &mcf->persistent_storage_backup);
            if (not_found) {
                return NGX_CONF_ERROR;
            } else {
                goto cleanup;
            }
        }

        backup_not_found = 1;

        if (not_found) {
            goto collection_check;
        }
    }

    if (!backup_not_found) {
        if (not_found) {
            ngx_conf_log_error(NGX_LOG_ALERT, cf, 0,
                               "backup persistent storage exists "
                               "while main persistent storage does not, "
                               "copying backup content into the main storage");
            copy_backup_file = 1;
        } else {
            if (ngx_fd_info(file.fd, &file_info) == NGX_FILE_ERROR) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                                   ngx_fd_info_n " \"%V\" failed", &file.name);
                goto cleanup;
            }

            if (ngx_file_mtime(&file_info_backup) > ngx_file_mtime(&file_info))
            {
                ngx_conf_log_error(NGX_LOG_ALERT, cf, 0,
                                   "backup persistent storage was modified "
                                   "later than main persistent storage, "
                                   "copying its content into the main storage");
                copy_backup_file = 1;

                /* close main persistent storage */

                if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
                    ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                                       ngx_close_file_n " \"%V\" failed",
                                       &file.name);
                }
            }
        }

        if (copy_backup_file) {

            do {
                /* check that backup is not corrupted */

                ngx_memzero(&backup_file, sizeof(ngx_file_t));

                backup_file.name = mcf->persistent_storage_backup;
                backup_file.log = cf->log;

                backup_file.fd = ngx_open_file(backup_file.name.data,
                                            NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);

                if (backup_file.fd == NGX_INVALID_FILE) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                                       ngx_open_file_n " \"%V\" failed",
                                       &backup_file.name);
                    break;
                }

                cleanup_backup = 1;

                file_size = (size_t) ngx_file_size(&file_info_backup);

                if (file_size == 0) {
                    break;
                }

                buf = ngx_alloc(file_size, cf->log);
                if (buf == NULL) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                       "failed to allocate memory to parse "
                                       "JSON data");
                    break;
                }

                n = ngx_read_file(&backup_file, buf, file_size, 0);

                if (n == NGX_ERROR) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                                       ngx_read_file_n " \"%V\" failed",
                                       &backup_file.name);
                    break;
                }

                if (ngx_close_file(backup_file.fd) == NGX_FILE_ERROR) {
                    ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                                       ngx_close_file_n " \"%V\" failed",
                                       &backup_file.name);
                }

                cleanup_backup = 0;

                if ((size_t) n != file_size) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                       ngx_read_file_n " \"%V\" returned only "
                                       "%z bytes instead of %z",
                                       &backup_file.name, n, file_size);
                    break;
                }

                jsmn_init(&jparse);

                jrc = jsmn_parse(&jparse, (char *) buf, file_size, NULL, 0);
                if (jrc < 0) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0, "JSON parse error: "
                                       "%d", jrc);
                    break;
                }

                jsz = jrc;
                jtok = ngx_alloc(sizeof(jsmntok_t) * jsz, cf->log);
                if (jtok == NULL) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                       "failed to allocate memory to parse "
                                       "JSON data");
                    break;
                }

                jsmn_init(&jparse);

                jrc = jsmn_parse(&jparse, (char *) buf, file_size, jtok, jsz);
                if (jrc < 0 || jtok[0].type != JSMN_OBJECT) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                       "JSON parse error: %d", jrc);
                    break;
                }

                copy_backup_started = 1;

                /* copy backup into the main storage */

                copy_file.size = ngx_file_size(&file_info_backup);
                copy_file.buf_size = 0;
                copy_file.access = NGX_FILE_DEFAULT_ACCESS;
                copy_file.time = ngx_file_mtime(&file_info_backup);
                copy_file.log = cf->log;

                if (ngx_copy_file(mcf->persistent_storage_backup.data,
                                  mcf->persistent_storage.data, &copy_file)
                    != NGX_OK)
                {
                    ngx_conf_log_error(NGX_LOG_ALERT, cf, 0,
                                       "failed to copy backup persistent "
                                       "storage into main persistent storage");
                    break;
                }

                copy_backup_ok = 1;

            } while (0);

            ngx_free(buf);
            ngx_free(jtok);

            if (cleanup_backup) {
                if (ngx_close_file(backup_file.fd) == NGX_FILE_ERROR) {
                    ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                                       ngx_close_file_n " \"%V\" failed",
                                       &backup_file.name);
                }
            }

            if (copy_backup_started) {
                if (!copy_backup_ok) {
                    return NGX_CONF_ERROR;
                }
            } else {
                if (file_size == 0) {
                    ngx_conf_log_error(NGX_LOG_ALERT, cf, 0,
                                       "backup file \"%V\" was not copied as "
                                       "it was empty", &backup_file.name);
                } else {
                    ngx_conf_log_error(NGX_LOG_ALERT, cf, 0,
                                       "backup file \"%V\" was not copied as "
                                       "it seemed to be corrupted",
                                       &backup_file.name);
                }
                if (not_found) {
                    goto collection_check;
                }
            }

            /* reopen main persistent storage */

            file.fd = ngx_open_file(file.name.data, NGX_FILE_RDONLY,
                                    NGX_FILE_OPEN, 0);

            if (file.fd == NGX_INVALID_FILE) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                                   ngx_open_file_n " \"%V\" failed",
                                   &file.name);
                return NGX_CONF_ERROR;
            }
        }
    }

    if (ngx_fd_info(file.fd, &file_info) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                           ngx_fd_info_n " \"%V\" failed", &file.name);
        goto cleanup;
    }

    file_size = (size_t) ngx_file_size(&file_info);

    if (file_size == 0) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "file \"%V\" is empty, delete it and run again",
                           &file.name);
        goto cleanup;
    }

    buf = ngx_pnalloc(cf->pool, file_size);
    if (buf == NULL) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "failed to allocate memory to parse JSON data");
        goto cleanup;
    }

    n = ngx_read_file(&file, buf, file_size, 0);

    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                           ngx_read_file_n " \"%V\" failed", &file.name);
        goto cleanup;
    }

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                           ngx_close_file_n " \"%V\" failed", &file.name);
    }

    if ((size_t) n != file_size) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           ngx_read_file_n " \"%V\" returned only %z bytes "
                           "instead of %z", &file.name, n, file_size);
        return NGX_CONF_ERROR;
    }

    jsmn_init(&jparse);

    jrc = jsmn_parse(&jparse, (char *) buf, file_size, NULL, 0);
    if (jrc < 0) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0, "JSON parse error: %d", jrc);
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "file \"%V\" is corrupted, delete it and run again",
                           &file.name);
        return NGX_CONF_ERROR;
    }

    jsz = jrc;
    jtok = ngx_palloc(cf->pool, sizeof(jsmntok_t) * jsz);
    if (jtok == NULL) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "failed to allocate memory to parse JSON data");
        return NGX_CONF_ERROR;
    }

    jsmn_init(&jparse);

    jrc = jsmn_parse(&jparse, (char *) buf, file_size, jtok, jsz);
    if (jrc < 0 || jtok[0].type != JSMN_OBJECT) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0, "JSON parse error: %d", jrc);
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "file \"%V\" is corrupted, delete it and run again",
                           &file.name);
        return NGX_CONF_ERROR;
    }

    mcf->persistent_collection.len = file_size;
    mcf->persistent_collection.data = buf;
    mcf->persistent_collection_tok = jtok;
    mcf->persistent_collection_size = jsz;

collection_check:

    if (cf->args->nelts > 2) {
        mcf->persistent_collection_check = ngx_parse_time(&value[2], 1);

        if (mcf->persistent_collection_check == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                               "bad check interval \"%V\"", &value[2]);
            return NGX_CONF_ERROR;
        }

        if (backup_not_found) {
            /* create backup file with the master process permissions,
             * later, in the module init function it must be chown()-ed */

            fd = ngx_open_file(mcf->persistent_storage_backup.data,
                               NGX_FILE_WRONLY, NGX_FILE_TRUNCATE,
                               NGX_FILE_DEFAULT_ACCESS);

            if (fd == NGX_INVALID_FILE) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                                   ngx_open_file_n " \"%V\" failed",
                                   &mcf->persistent_storage_backup);
                return NGX_CONF_ERROR;
            }

            if (ngx_close_file(fd) == NGX_FILE_ERROR) {
                ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                                   ngx_close_file_n " \"%V\" failed",
                                   &mcf->persistent_storage_backup);
            }

            mcf->persistent_storage_backup_requires_init = 1;
        }
    }

    return NGX_CONF_OK;

cleanup:

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                           ngx_close_file_n " \"%V\" failed", &file.name);
    }

    return NGX_CONF_ERROR;
}


static ngx_int_t
ngx_http_cnt_init_persistent_storage(ngx_cycle_t *cycle)
{
    ngx_http_cnt_main_conf_t      *mcf;
    ngx_core_conf_t               *ccf;
    ngx_file_info_t                file_info;

    mcf = ngx_http_cycle_get_module_main_conf(cycle,
                                              ngx_http_custom_counters_module);

    if (mcf->persistent_collection_check == 0
        || mcf->persistent_storage_backup.len == 0)
    {
        return NGX_OK;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    if (!ccf || ccf->user == (ngx_uid_t) NGX_CONF_UNSET_UINT) {
        return NGX_OK;
    }

    if (ngx_file_info(mcf->persistent_storage_backup.data, &file_info)
        == NGX_FILE_ERROR)
    {
        if (mcf->persistent_storage_backup_requires_init) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                          ngx_file_info_n " \"%V\" failed",
                          &mcf->persistent_storage_backup);
            return NGX_ERROR;
        }

        file_info.st_uid = ccf->user;
    }

    if ((mcf->persistent_storage_backup_requires_init
         || file_info.st_uid != ccf->user)
        && chown((const char *) mcf->persistent_storage_backup.data,
                 ccf->user, -1) == -1)
    {
        ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                      "chown(\"%s\", %d) failed",
                      mcf->persistent_storage_backup.data, ccf->user);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_load_persistent_counters(ngx_log_t *log, ngx_str_t collection,
                                      jsmntok_t *collection_tok,
                                      int collection_size, ngx_str_t cnt_set,
                                      ngx_array_t *vars,
                                      ngx_atomic_int_t *shm_data)
{
    ngx_int_t                      i, j, k;
    ngx_http_cnt_set_var_data_t   *elts;
    ngx_int_t                      nelts;
    ngx_int_t                      idx, val;
    ngx_str_t                      tok;
    ngx_uint_t                     skip;

    nelts = vars->nelts;
    if (nelts == 0) {
        return NGX_OK;
    }

    elts = vars->elts;

    for (i = 1; i < collection_size; i++) {
        if (collection_tok[i].type != JSMN_STRING) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "unexpected structure of JSON data: "
                          "key is not a string");
            return NGX_ERROR;
        }

        skip = 0;
        tok.len = collection_tok[i].end - collection_tok[i].start;
        tok.data = &collection.data[collection_tok[i].start];

        i++;

        if (tok.len != cnt_set.len
            || ngx_strncmp(tok.data, cnt_set.data, tok.len) != 0)
        {
            skip = 1;
        }

        if (i >= collection_size || collection_tok[i].type != JSMN_OBJECT) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "unexpected structure of JSON data: "
                          "value is not an object");
            return NGX_ERROR;
        }

        if (skip) {
            i += 2 * collection_tok[i].size;
            continue;
        }

        for (j = 0; j < collection_tok[i].size; j++) {
            idx = i + 1 + j * 2;

            if (collection_tok[idx].type != JSMN_STRING
                && collection_tok[idx + 1].type != JSMN_PRIMITIVE)
            {
                ngx_log_error(NGX_LOG_ERR, log, 0,
                              "unexpected structure of JSON data: "
                              "value is not a string / primitive pair");
                return NGX_ERROR;
            }

            tok.len = collection_tok[idx].end - collection_tok[idx].start;
            tok.data = &collection.data[collection_tok[idx].start];
            for (k = 0; k < nelts; k++) {
                if (elts[k].name.len == tok.len
                    && ngx_strncmp(elts[k].name.data, tok.data, tok.len) == 0)
                {
                    tok.len =
                            collection_tok[idx + 1].end -
                            collection_tok[idx + 1].start;
                    tok.data =
                            &collection.data[collection_tok[idx + 1].start];

                    val = ngx_atoi(tok.data, tok.len);
                    if (val == NGX_ERROR) {
                        ngx_log_error(NGX_LOG_ERR, log, 0,
                                      "not a number \"%V\"", &tok);
                        return NGX_ERROR;
                    }

                    shm_data[elts[k].idx] = val;

                    break;
                }
            }
        }

        i += j * 2;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_write_persistent_counters(ngx_http_request_t *r,
                                       ngx_cycle_t *cycle, ngx_uint_t backup)
{
    ngx_http_cnt_main_conf_t      *mcf;
    ngx_log_t                     *log;
    ngx_str_t                      collection;
    ngx_file_t                     file;

    if (r == NULL) {
        if (cycle == NULL) {
            return NGX_ERROR;
        }
        mcf = ngx_http_cycle_get_module_main_conf(cycle,
                                            ngx_http_custom_counters_module);
        log = cycle->log;
    } else {
        mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);
        log = r->connection->log;
    }

    if (mcf->persistent_storage.len == 0) {
        return NGX_OK;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));

    file.name = backup ? mcf->persistent_storage_backup :
            mcf->persistent_storage;
    file.log = log;

    file.fd = ngx_open_file(file.name.data, NGX_FILE_WRONLY,
                            NGX_FILE_TRUNCATE, NGX_FILE_DEFAULT_ACCESS);

    if (file.fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      ngx_open_file_n " \"%V\" failed", &file.name);
        return NGX_ERROR;
    }

    if (ngx_http_cnt_build_collection(r, cycle, &collection, 1) != NGX_OK) {
        goto cleanup;
    }

    if (ngx_write_file(&file, collection.data, collection.len, 0) == NGX_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      ngx_write_fd_n " \"%V\" failed", &file.name);
    }

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      ngx_close_file_n " \"%V\" failed", &file.name);
        return NGX_ERROR;
    }

    return NGX_OK;

cleanup:

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      ngx_close_file_n " \"%V\" failed", &file.name);
    }

    return NGX_ERROR;
}

#endif

