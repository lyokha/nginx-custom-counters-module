/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_histogram.h
 *
 *    Description:  histograms
 *
 *        Version:  4.0
 *        Created:  26.06.2020 15:35:01
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#ifndef NGX_HTTP_CUSTOM_COUNTERS_HISTOGRAM_H
#define NGX_HTTP_CUSTOM_COUNTERS_HISTOGRAM_H

#include <ngx_core.h>
#include <ngx_http.h>


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


char *ngx_http_cnt_histogram(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

#endif /* NGX_HTTP_CUSTOM_COUNTERS_HISTOGRAM_H */
