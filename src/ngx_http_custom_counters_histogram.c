/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_histogram.c
 *
 *    Description:  histograms
 *
 *        Version:  4.0
 *        Created:  26.06.2020 15:33:11
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#include "ngx_http_custom_counters_module.h"
#include "ngx_http_custom_counters_histogram.h"


static const ngx_int_t  ngx_http_cnt_histogram_max_bins = 32;


typedef enum {
    ngx_http_cnt_histogram_sum,
    ngx_http_cnt_histogram_err
} ngx_http_cnt_histogram_special_var_e;


static ngx_int_t ngx_http_cnt_get_histogram_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static ngx_int_t ngx_http_cnt_get_histogram_inc_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data);
static ngx_int_t ngx_http_cnt_histogram_special_var(ngx_conf_t *cf, void *conf,
    ngx_http_cnt_srv_conf_t *scf, ngx_str_t *counter_name,
    ngx_str_t *counter_op_value, ngx_str_t base_name, ngx_int_t idx,
    ngx_http_cnt_set_histogram_data_t *data,
    ngx_http_cnt_histogram_special_var_e type);


char *
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

