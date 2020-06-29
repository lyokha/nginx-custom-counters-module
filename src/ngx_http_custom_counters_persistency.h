/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_persistency.h
 *
 *    Description: persistent counters 
 *
 *        Version:  4.0
 *        Created:  26.06.2020 14:25:51
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#ifndef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY_H
#define NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY_H

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY

#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_custom_counters_forward_jsmntok.h"


char *ngx_http_cnt_counters_persistent_storage(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
ngx_int_t ngx_http_cnt_init_persistent_storage(ngx_cycle_t *cycle);
ngx_int_t ngx_http_cnt_load_persistent_counters(ngx_log_t* log,
    ngx_str_t collection, jsmntok_t *collection_tok, int collection_size,
    ngx_str_t cnt_set, ngx_array_t *vars, ngx_atomic_int_t *shm_data);
ngx_int_t ngx_http_cnt_write_persistent_counters(ngx_http_request_t *r,
    ngx_cycle_t *cycle, ngx_uint_t backup);

#endif

#endif /* NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY_H */

