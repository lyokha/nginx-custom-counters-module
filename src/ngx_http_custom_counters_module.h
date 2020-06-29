/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_module.h
 *
 *    Description:  nginx module for shared custom counters
 *
 *        Version:  4.0
 *        Created:  26.06.2020 14:31:21
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

#include <ngx_core.h>
#include <ngx_http.h>

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY
#include "ngx_http_custom_counters_forward_jsmntok.h"
#endif


typedef struct {
    ngx_int_t                   self;
    ngx_int_t                   idx;
    ngx_str_t                   name;
} ngx_http_cnt_set_var_data_t;


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
    ngx_uint_t                  cnt_set;
    ngx_str_t                   cnt_set_id;
    ngx_str_t                   unreachable_cnt_mark;
    ngx_flag_t                  survive_reload;
} ngx_http_cnt_srv_conf_t;


typedef struct {
    ngx_array_t                 cnt_sets;
    ngx_str_t                   histograms;
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


ngx_int_t ngx_http_cnt_build_collection(ngx_http_request_t *r,
    ngx_cycle_t *cycle, ngx_str_t *collection, ngx_uint_t survive_reload_only);
ngx_int_t ngx_http_cnt_counter_set_init(ngx_conf_t *cf,
    ngx_http_cnt_main_conf_t *mcf, ngx_http_cnt_srv_conf_t *scf);
char *ngx_http_cnt_counter_impl(ngx_conf_t *cf, ngx_command_t *cmd, void *conf,
    ngx_uint_t early);
ngx_int_t ngx_http_cnt_var_data_init(ngx_conf_t *cf,
    ngx_http_cnt_srv_conf_t *scf, ngx_http_variable_t *v, ngx_int_t idx,
    ngx_http_get_variable_pt handler, ngx_int_t bin_idx);


extern ngx_module_t  ngx_http_custom_counters_module;

#endif /* NGX_HTTP_CUSTOM_COUNTERS_MODULE_H */

