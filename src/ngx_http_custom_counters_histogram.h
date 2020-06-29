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


char *ngx_http_cnt_histogram(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
ngx_int_t ngx_http_cnt_init_histograms(ngx_cycle_t *cycle);
ngx_int_t ngx_http_cnt_histograms(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
char *ngx_http_cnt_map_range_index(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

#endif /* NGX_HTTP_CUSTOM_COUNTERS_HISTOGRAM_H */

