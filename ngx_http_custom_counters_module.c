/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_module.c
 *
 *    Description:  nginx module for shared custom counters
 *
 *        Version:  1.0
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

#define STRLEN(X) (sizeof(X) - 1)


static const char ngx_http_cnt_shm_name_prefix[] = "custom_counters_";
static const char ngx_http_cnt_shm_name_prefix_len =
    STRLEN(ngx_http_cnt_shm_name_prefix);


typedef enum
{
    ngx_http_cnt_op_set,
    ngx_http_cnt_op_inc
} ngx_http_cnt_op_e;


typedef struct
{
    ngx_http_cnt_op_e          op;
    ngx_int_t                  self;
    ngx_int_t                  idx;
    ngx_int_t                  value;
    ngx_uint_t                 early;
} ngx_http_cnt_data_t;


typedef struct
{
    ngx_str_t                  name;
    ngx_array_t                vars;
    ngx_shm_zone_t            *data;
} ngx_http_cnt_set_t;


typedef struct
{
    ngx_http_cnt_set_t        *cnt_set;
    ngx_int_t                  self;
} ngx_http_cnt_var_data_t;


typedef struct {
    ngx_atomic_t              *data;
} ngx_http_cnt_shm_data_t;


typedef struct {
    ngx_array_t                cnt_sets;
} ngx_http_cnt_main_conf_t;


typedef struct {
    ngx_http_cnt_set_t        *cnt_set;
} ngx_http_cnt_srv_conf_t;


typedef struct {
    ngx_array_t                cnt_data;
} ngx_http_cnt_loc_conf_t;


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
static char *ngx_http_cnt_counter_impl(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf, ngx_uint_t early);
static char *ngx_http_cnt_counter(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_cnt_early_counter(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_cnt_merge(ngx_conf_t *cf, ngx_array_t *dst,
    ngx_http_cnt_data_t *cnt_data);
static ngx_int_t ngx_http_cnt_rewrite_phase_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_cnt_response_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_cnt_update(ngx_http_request_t *r, ngx_uint_t early);


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;


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
    ngx_http_server_name_t      *sn;
    ngx_http_cnt_set_t          *cnt_set;
    ngx_http_handler_pt         *h;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    cscfp = cmcf->servers.elts;
    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_custom_counters_module);
    cnt_set = mcf->cnt_sets.elts;

    for (i = 0; i < cmcf->servers.nelts; i++) {
        scf = cscfp[i]->ctx->srv_conf[
                ngx_http_custom_counters_module.ctx_index];
        if (scf->cnt_set != NULL || cscfp[i]->server_names.nelts == 0) {
            continue;
        }
        sn = &((ngx_http_server_name_t *) cscfp[i]->server_names.elts)[
                cscfp[i]->server_names.nelts - 1];
        for (j = 0; j < mcf->cnt_sets.nelts; j++) {
            if (cnt_set[j].name.len == sn->name.len
                && ngx_strncmp(cnt_set[j].name.data, sn->name.data,
                               sn->name.len) == 0)
            {
                scf->cnt_set = cnt_set;
                break;
            }
        }
    }

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);

    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_cnt_rewrite_phase_handler;

    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter  = ngx_http_cnt_response_header_filter;

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
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_cnt_srv_conf_t));
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
    ngx_http_cnt_srv_conf_t  *prev = parent;
    ngx_http_cnt_srv_conf_t  *conf = child;

    if (prev->cnt_set != NULL && conf->cnt_set != NULL
        && prev->cnt_set != conf->cnt_set)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "merged server configurations have different "
                           "server names");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_cnt_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_cnt_loc_conf_t  *prev = parent;
    ngx_http_cnt_loc_conf_t  *conf = child;

    ngx_uint_t                i, size;
    ngx_http_cnt_data_t      *cnt_data;
    ngx_array_t               child_data;

    if (prev->cnt_data.nelts == 0 && conf->cnt_data.nelts == 0) {
        return NGX_CONF_OK;
    }

    size = ngx_max(prev->cnt_data.nelts, conf->cnt_data.nelts);

    if (ngx_array_init(&child_data, cf->pool, size,
                       sizeof(ngx_http_cnt_data_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
    for (i = 0; i < prev->cnt_data.nelts; i++) {
        cnt_data = ngx_array_push(&child_data);
        if (cnt_data == NULL) {
            return NGX_CONF_ERROR;
        }
        *cnt_data = ((ngx_http_cnt_data_t *) prev->cnt_data.elts)[i];
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
    ngx_http_cnt_set_t  *cnt_set = shm_zone->data;

    ngx_slab_pool_t     *shpool;
    ngx_atomic_uint_t   *shm_data;
    size_t               size;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    size = sizeof(ngx_atomic_uint_t) * cnt_set->vars.nelts;

    shm_data = ngx_slab_alloc(shpool, size);
    if (shm_data == NULL) {
        return NGX_ERROR;
    }

    shpool->data = shm_data;
    shm_zone->data = shm_data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cnt_get_value(ngx_http_request_t *r, ngx_http_variable_value_t *v,
                       uintptr_t  data)
{
    ngx_array_t                       *self = (ngx_array_t *) data;

    ngx_uint_t                         i;
    ngx_http_cnt_srv_conf_t           *scf;
    ngx_shm_zone_t                    *shm;
    ngx_http_cnt_var_data_t           *var_data;
    ngx_atomic_t                      *shm_data;
    ngx_int_t                          idx = NGX_ERROR;
    u_char                            *buf, *last;
    static const ngx_uint_t            bufsz = 32;

    if (self == NULL) {
        return NGX_ERROR;
    }
    var_data = self->elts;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NULL || ((shm = scf->cnt_set->data) == NULL)) {
        return NGX_ERROR;
    }

    for (i = 0; i < self->nelts; i++)
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

    shm_data = shm->data;
    buf = ngx_pnalloc(r->pool, bufsz);
    last = ngx_snprintf(buf, bufsz, "%d", shm_data[idx]);

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
    ngx_http_server_name_t        *sn;
    ngx_http_cnt_set_t            *cnt_sets, *cnt_set;
    ngx_http_cnt_data_t            cnt_data;
    ngx_str_t                      cnt_name;
    ngx_uint_t                    *vars, *var;
    ngx_http_cnt_var_data_t       *var_data;
    ngx_array_t                   *v_data;
    ngx_int_t                      idx = NGX_ERROR, v_idx;
    ngx_int_t                      val = 1;

    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_custom_counters_module);
    scf = ngx_http_conf_get_module_srv_conf(cf,
                                            ngx_http_custom_counters_module);
    cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);

    if (cscf->server_names.nelts == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "custom counters require server name");
        return NGX_CONF_ERROR;
    }

    sn = &((ngx_http_server_name_t *) cscf->server_names.elts)[
            cscf->server_names.nelts - 1];

    if (sn->name.len == 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "custom counters require not empty "
                           "last server name");
        return NGX_CONF_ERROR;
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
    if (scf->cnt_set == NULL) {
        for (i = 0; i < mcf->cnt_sets.nelts; i++) {
            if (cnt_sets[i].name.len == sn->name.len
                && ngx_strncmp(cnt_sets[i].name.data, sn->name.data,
                               sn->name.len) == 0)
            {
                scf->cnt_set = &cnt_sets[i];
            }
        }
    }
    if (scf->cnt_set == NULL) {
        cnt_set = ngx_array_push(&mcf->cnt_sets);
        if (cnt_set == NULL) {
            return NGX_CONF_ERROR;
        }
        cnt_set->name = sn->name;
        cnt_name.len = ngx_http_cnt_shm_name_prefix_len + sn->name.len;
        cnt_name.data = ngx_pnalloc(cf->pool, cnt_name.len);
        if (cnt_name.data == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_memcpy(cnt_name.data,
                   ngx_http_cnt_shm_name_prefix,
                   ngx_http_cnt_shm_name_prefix_len);
        ngx_memcpy(cnt_name.data + ngx_http_cnt_shm_name_prefix_len,
                   sn->name.data,
                   sn->name.len);
        cnt_set->data = ngx_shared_memory_add(cf, &cnt_name, 8 * ngx_pagesize,
                                              &ngx_http_custom_counters_module);
        if (cnt_set->data == NULL) {
            return NGX_CONF_ERROR;
        }
        cnt_set->data->init = ngx_http_cnt_shm_init;
        cnt_set->data->data = cnt_set;
        if (ngx_array_init(&cnt_set->vars, cf->pool, 1,
                           sizeof(ngx_uint_t)) != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }

        scf->cnt_set = cnt_set;
    }

    v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }
    v_idx = ngx_http_get_variable_index(cf, &value[1]);
    if (v_idx == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    vars = scf->cnt_set->vars.elts;
    for (i = 0; i < scf->cnt_set->vars.nelts; i++)
    {
        if (vars[i] == (ngx_uint_t) v_idx) {
            idx = i;
            break;
        }
    }
    if (idx == NGX_ERROR) {
        var = ngx_array_push(&scf->cnt_set->vars);
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
        v_data = ngx_pnalloc(cf->pool, sizeof(ngx_array_t));
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

    if (cf->args->nelts == 4) {
        ngx_uint_t  negative = 0;

        if (value[3].data[0] == '-') {
            value[3].len--;
            value[3].data++;
            negative = 1;
        }
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
ngx_http_cnt_merge(ngx_conf_t *cf, ngx_array_t *dst,
                   ngx_http_cnt_data_t *cnt_data)
{
    ngx_http_cnt_data_t               *data = dst->elts;

    ngx_uint_t                         i;
    ngx_http_cnt_data_t               *new_data;
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
                               "in same scope");
            return NGX_CONF_ERROR;
        }
        if (cnt_data->op == ngx_http_cnt_op_inc) {
            if (new_data->op == ngx_http_cnt_op_set) {
                new_data->op = ngx_http_cnt_op_set;
            }
            new_data->value += cnt_data->value;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_cnt_rewrite_phase_handler(ngx_http_request_t *r)
{
    if (r != r->main) {
        return NGX_DECLINED;
    }

    (void) ngx_http_cnt_update(r, 1);

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_cnt_response_header_filter(ngx_http_request_t *r)
{
    if (r != r->main) {
        return ngx_http_next_header_filter(r);
    }

    (void) ngx_http_cnt_update(r, 0);

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_cnt_update(ngx_http_request_t *r, ngx_uint_t early)
{
    ngx_uint_t                     i;
    ngx_http_cnt_srv_conf_t       *scf;
    ngx_http_cnt_loc_conf_t       *lcf;
    ngx_http_cnt_data_t           *cnt_data;
    ngx_atomic_t                  *data, *dst;
    ngx_slab_pool_t               *shpool;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_custom_counters_module);
    if (scf->cnt_set == NULL) {
        return NGX_DECLINED;
    }
    data = scf->cnt_set->data->data;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_counters_module);
    cnt_data = lcf->cnt_data.elts;

    for (i = 0; i < lcf->cnt_data.nelts; i++) {
        if (cnt_data[i].early != early) {
            continue;
        }
        dst = &data[cnt_data[i].idx];
        if (cnt_data[i].op == ngx_http_cnt_op_set) {
            shpool = (ngx_slab_pool_t *) scf->cnt_set->data->shm.addr;
            ngx_shmtx_lock(&shpool->mutex);
            *dst = cnt_data[i].value;
            ngx_shmtx_unlock(&shpool->mutex);

        } else if (cnt_data[i].op == ngx_http_cnt_op_inc) {
            ngx_atomic_fetch_add(dst, cnt_data[i].value);
        }
    }

    return NGX_OK;
}
