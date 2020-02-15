/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_module.h
 *
 *    Description:  nginx module for shared custom counters
 *
 *        Version:  1.0
 *        Created:  14.02.2020 22:04:53
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#ifndef NGX_HTTP_CUSTOM_COUNTERS_MODULE_H
#define NGX_HTTP_CUSTOM_COUNTERS_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


ngx_int_t ngx_http_cnt_build_collection(ngx_http_request_t *r,
    ngx_cycle_t *cycle, ngx_str_t *collection);

#endif /* NGX_HTTP_CUSTOM_COUNTERS_MODULE_H */

