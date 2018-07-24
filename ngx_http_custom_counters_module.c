/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_module.c
 *
 *    Description:  nginx module for shared custom counters
 *
 *        Version:  1.2
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


static const ngx_str_t  ngx_http_cnt_shm_name_prefix =
    ngx_string("custom_counters_");


typedef enum
{
    ngx_http_cnt_op_set,
    ngx_http_cnt_op_inc
} ngx_http_cnt_op_e;


typedef struct
{
    ngx_int_t                  self;
    ngx_uint_t                 negative;
} ngx_http_cnt_rt_vars_data_t;


typedef struct
{
    ngx_http_cnt_op_e          op;
    ngx_int_t                  self;
    ngx_int_t                  idx;
    ngx_int_t                  value;
    ngx_array_t                rt_vars;
    ngx_uint_t                 early;
} ngx_http_cnt_data_t;


typedef struct
{
    ngx_str_t                  name;
    ngx_array_t                vars;
    ngx_shm_zone_t            *zone;
    ngx_uint_t                 survive_reload;
} ngx_http_cnt_set_t;


typedef struct
{
    ngx_uint_t                 cnt_set;
    ngx_int_t                  self;
} ngx_http_cnt_var_data_t;


typedef struct {
    ngx_array_t                cnt_sets;
} ngx_http_cnt_main_conf_t;


typedef struct {
    ngx_uint_t                 cnt_set;
    ngx_str_t                  cnt_set_id;
    ngx_flag_t                 survive_reload;
} ngx_http_cnt_srv_conf_t;


typedef struct {
    ngx_array_t                cnt_data;
} ngx_http_cnt_loc_conf_t;


typedef struct
{
    ngx_array_t               *cnt_sets;
    ngx_uint_t                 cnt_set;
} ngx_http_cnt_shm_data_t;


static ngx_int_t ngx_http_cnt_init(ngx_conf_t *cf);
static void *ngx_http_cnt_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_cnt_create_srv_conf(ngx_conf_t *cf);
static void *ngx_http_cnt_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_cnt_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_cnt_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_cnt_shm_init(ngx_shm_zone_t *shm_zone, void *data);
static ngx_int_t ngx_http_cnt_get_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static char *ngx_http_cnt_counter_impl(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf, ngx_uint_t early);
static char *ngx_http_cnt_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_early_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_counter_set_id(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_cnt_merge(ngx_conf_t *cf, ngx_array_t *dst,
    ngx_http_cnt_data_t *cnt_data);
static ngx_inline ngx_int_t ngx_http_cnt_phase_handler_impl(
    ngx_http_request_t *r, ngx_uint_t early);
static ngx_int_t ngx_http_cnt_rewrite_phase_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_cnt_log_phase_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_cnt_update(ngx_http_request_t *r, ngx_uint_t early);


static ngx_command_t ngx_http_cnt_commands[] = {

    { ngx_string("counter"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE23,
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

      ngx_null_command
};


static ngx_http_module_t  ngx_http_cnt_module_ctx = {
    NULL,                                    /* preconfiguration */
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
    NULL,                                    /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    NULL,                                    /* exit process */
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


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
    ngx_http_cnt_loc_conf_t  *lcf;

    lcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cnt_loc_conf_t));
    if (lcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&lcf->cnt_data, cf->pool, 1,
                       sizeof(ngx_http_cnt_data_t)) != NGX_OK)
    {
        return NULL;
    }

    return lcf;
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
    ngx_http_cnt_loc_conf_t      *prev = parent;
    ngx_http_cnt_loc_conf_t      *conf = child;

    ngx_uint_t                    i, j, size;
    ngx_http_cnt_data_t          *cnt_data, *prev_cnt_data;
    ngx_array_t                   child_data;
    ngx_http_cnt_rt_vars_data_t  *rt_var;

    if (prev->cnt_data.nelts == 0 && conf->cnt_data.nelts == 0) {
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
        if (ngx_array_init(&cnt_data->rt_vars, cf->pool, size,
                           sizeof(ngx_http_cnt_rt_vars_data_t)) != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
        for (j = 0; j < size; j++) {
            rt_var = ngx_array_push(&cnt_data->rt_vars);
            if (rt_var == NULL) {
                return NGX_CONF_ERROR;
            }
            *rt_var = ((ngx_http_cnt_rt_vars_data_t *)
                                prev_cnt_data[i].rt_vars.elts)[j];
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
                              "custom counters set \"%V\" cannot survive reload "
                              "because its size has changed", &cnt_set->name);
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

    /* FIXME: this is not always safe: too slow workers may write in recently
     * allocated areas when nginx reloads its configuration too fast and having 
     * been already freed areas get reused */
    if (oshm_data != NULL) {
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
    static const ngx_uint_t            bufsz = 32;

    if (v_data == NULL) {
        return NGX_ERROR;
    }
    var_data = v_data->elts;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        return NGX_ERROR;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_custom_counters_module);
    cnt_sets = mcf->cnt_sets.elts;
    cnt_set = &cnt_sets[scf->cnt_set];

    shm = cnt_set->zone;
    if (shm == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < v_data->nelts; i++)
    {
        if (var_data[i].cnt_set != scf->cnt_set) {
            continue;
        }

        idx = var_data[i].self;
        break;
    }
    if (idx == NGX_ERROR) {
        return NGX_ERROR;
    }

    shm_data = (volatile ngx_atomic_int_t *) shm->data + 1;
    buf = ngx_pnalloc(r->pool, bufsz);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    last = ngx_snprintf(buf, bufsz, "%A", shm_data[idx]);

    v->len          = last - buf;
    v->data         = buf;
    v->valid        = 1;
    v->no_cacheable = 0;
    v->not_found    = 0;

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
    ngx_http_core_srv_conf_t      *cscf;
    ngx_str_t                     *value;
    ngx_http_variable_t           *v;
    ngx_http_cnt_set_t            *cnt_sets, *cnt_set;
    ngx_http_cnt_data_t            cnt_data;
    ngx_str_t                      cnt_set_id, cnt_name;
    ngx_uint_t                    *vars, *var;
    ngx_http_cnt_var_data_t       *var_data;
    ngx_array_t                   *v_data;
    ngx_http_cnt_rt_vars_data_t   *rt_var;
    ngx_http_cnt_shm_data_t       *shm_data;
    ngx_int_t                      idx = NGX_ERROR, v_idx;
    ngx_int_t                      val = 1;
    ngx_uint_t                     negative = 0;

    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_custom_counters_module);
    scf = ngx_http_conf_get_module_srv_conf(cf,
                                            ngx_http_custom_counters_module);
    cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);

    cnt_set_id = scf->cnt_set_id;

    if (cnt_set_id.len == 0) {
        if (cscf->server_names.nelts == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "custom counters require directive "
                               "\"counter_set_id\" or server name");
            return NGX_CONF_ERROR;
        }
        cnt_set_id = ((ngx_http_server_name_t *) cscf->server_names.elts)[
                            cscf->server_names.nelts - 1].name;
        if (cnt_set_id.len == 0)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "custom counters require directive "
                               "\"counter_set_id\" or non-empty last server "
                               "name");
            return NGX_CONF_ERROR;
        }
    }

    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }
    value[1].len--;
    value[1].data++;

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
    if (scf->cnt_set == NGX_CONF_UNSET_UINT) {
        cnt_set = ngx_array_push(&mcf->cnt_sets);
        if (cnt_set == NULL) {
            return NGX_CONF_ERROR;
        }
        cnt_set->name = cnt_set_id;
        cnt_name.len = ngx_http_cnt_shm_name_prefix.len + cnt_set_id.len;
        cnt_name.data = ngx_pnalloc(cf->pool, cnt_name.len);
        if (cnt_name.data == NULL) {
            return NGX_CONF_ERROR;
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
            return NGX_CONF_ERROR;
        }

        if (ngx_array_init(&cnt_set->vars, cf->pool, 1,
                           sizeof(ngx_uint_t)) != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }

        shm_data = ngx_palloc(cf->pool, sizeof(ngx_http_cnt_shm_data_t));
        if (shm_data == NULL) {
            return NGX_CONF_ERROR;
        }

        scf->cnt_set = mcf->cnt_sets.nelts - 1;
        shm_data->cnt_sets = &mcf->cnt_sets;
        shm_data->cnt_set = scf->cnt_set;

        cnt_set->zone->init = ngx_http_cnt_shm_init;
        cnt_set->zone->data = shm_data;
        cnt_set->survive_reload = 0;
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
    for (i = 0; i < cnt_set->vars.nelts; i++)
    {
        if (vars[i] == (ngx_uint_t) v_idx) {
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
        *var = v_idx;
    }
    if (v->get_handler != NULL && v->get_handler != ngx_http_cnt_get_value) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "custom counter variable has a different setter");
        return NGX_CONF_ERROR;
    }
    if ((ngx_array_t *) v->data == NULL) {
        v_data = ngx_palloc(cf->pool, sizeof(ngx_array_t));
        if (v_data == NULL) {
            return NGX_CONF_ERROR;
        }
        if (ngx_array_init(v_data, cf->pool, 1,
                           sizeof(ngx_http_cnt_var_data_t)) != NGX_OK)
        {
            return NULL;
        }
        var_data = ngx_array_push(v_data);
        if (var_data == NULL) {
            return NGX_CONF_ERROR;
        }
        var_data->cnt_set = scf->cnt_set;
        var_data->self = idx;

        if (v->get_handler == NULL) {
            v->get_handler = ngx_http_cnt_get_value;
        }
        v->data = (uintptr_t) v_data;
    } else {
        ngx_uint_t                found = 0;
        ngx_http_cnt_var_data_t  *var_data_elts;

        v_data = (ngx_array_t *) v->data;
        var_data_elts = v_data->elts;

        for (i = 0; i < v_data->nelts; i++) {
            if (var_data_elts[i].cnt_set == scf->cnt_set) {
                found = 1;
                break;
            }
        }
        if (!found) {
            var_data = ngx_array_push(v_data);
            if (var_data == NULL) {
                return NGX_CONF_ERROR;
            }
            var_data->cnt_set = scf->cnt_set;
            var_data->self = idx;
        }
    }

    if (ngx_array_init(&cnt_data.rt_vars, cf->pool, 1,
                       sizeof(ngx_http_cnt_rt_vars_data_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

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
            rt_var = ngx_array_push(&cnt_data.rt_vars);
            if (rt_var == NULL) {
                return NGX_CONF_ERROR;
            }
            rt_var->self = val;
            rt_var->negative = negative;
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

    if (value[2].len == 3 && ngx_strncmp(value[2].data, "set", 3) == 0) {
        cnt_data.op = ngx_http_cnt_op_set;
    } else if (value[2].len == 3 && ngx_strncmp(value[2].data, "inc", 3) == 0) {
        cnt_data.op = ngx_http_cnt_op_inc;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown counter operation: \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    cnt_data.self  = v_idx;
    cnt_data.idx   = idx;
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
ngx_http_cnt_merge(ngx_conf_t *cf, ngx_array_t *dst,
                   ngx_http_cnt_data_t *cnt_data)
{
    ngx_http_cnt_data_t               *data = dst->elts;

    ngx_uint_t                         i;
    ngx_http_cnt_data_t               *new_data;
    ngx_http_cnt_rt_vars_data_t       *rt_var;
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
            if (new_data->op == ngx_http_cnt_op_set) {
                new_data->op = ngx_http_cnt_op_set;
            }
            new_data->value += cnt_data->value;
            for (i = 0; i < cnt_data->rt_vars.nelts; i++) {
                rt_var = ngx_array_push(&new_data->rt_vars);
                if (rt_var == NULL) {
                    return NGX_CONF_ERROR;
                }
                *rt_var = ((ngx_http_cnt_rt_vars_data_t *)
                                    cnt_data->rt_vars.elts)[i];
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
    ngx_http_cnt_rt_vars_data_t   *rt_vars;
    ngx_http_variable_value_t     *var;
    ngx_http_cnt_set_t            *cnt_sets, *cnt_set;
    ngx_int_t                      value, val;
    ngx_str_t                      base_var;
    ngx_http_variable_t           *v;
    ngx_uint_t                     negative;

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
        for (j = 0; j < cnt_data[i].rt_vars.nelts; j++) {
            var = ngx_http_get_indexed_variable(r, rt_vars[j].self);
            if (var == NULL || !var->valid || var->not_found) {
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
                continue;
            }
            if (negative) {
                val = -val;
            }
            value += rt_vars[j].negative ? -val : val;
        }
        if (cnt_data[i].op == ngx_http_cnt_op_set) {
            shpool = (ngx_slab_pool_t *) cnt_set->zone->shm.addr;
            ngx_shmtx_lock(&shpool->mutex);
            *dst = value;
            ngx_shmtx_unlock(&shpool->mutex);
        } else if (cnt_data[i].op == ngx_http_cnt_op_inc) {
            if (value != 0) {
                (void) ngx_atomic_fetch_add(dst, value);
            }
        }
    }

    return NGX_OK;
}

