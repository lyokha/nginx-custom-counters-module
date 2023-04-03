/*
 * =============================================================================
 *
 *       Filename:  ngx_http_custom_counters_persistency.c
 *
 *    Description:  persistent counters
 *
 *        Version:  4.0
 *        Created:  26.06.2020 14:18:48
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#ifdef NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY

#include "ngx_http_custom_counters_module.h"
#include "ngx_http_custom_counters_persistency.h"

#define JSMN_STATIC
#define JSMN_STRICT
#include <jsmn.h>


char *
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
            ngx_str_set(&dir_name, "./");
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
            ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
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
            ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
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
                ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
                                   "backup persistent storage was modified "
                                   "later than main persistent storage, "
                                   "copying its content into the main storage");
                copy_backup_file = 1;

                /* close main persistent storage */

                if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
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
                    ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
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
                if (jrc < 0) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                       "JSON parse error: %d", jrc);
                    break;
                }
                if (jtok[0].type != JSMN_OBJECT) {
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                                       "unexpected structure of JSON data: "
                                       "the whole data is not an object");
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
                    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
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
                    ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
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
                    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
                                       "backup file \"%V\" was not copied as "
                                       "it was empty", &backup_file.name);
                } else {
                    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
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
        ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
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
    if (jrc < 0) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0, "JSON parse error: %d", jrc);
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "file \"%V\" is corrupted, delete it and run again",
                           &file.name);
        return NGX_CONF_ERROR;
    }
    if (jtok[0].type != JSMN_OBJECT) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "unexpected structure of JSON data: "
                           "the whole data is not an object");
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
                ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                                   ngx_close_file_n " \"%V\" failed",
                                   &mcf->persistent_storage_backup);
            }

            mcf->persistent_storage_backup_requires_init = 1;
        }
    }

    return NGX_CONF_OK;

cleanup:

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                           ngx_close_file_n " \"%V\" failed", &file.name);
    }

    return NGX_CONF_ERROR;
}


ngx_int_t
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


ngx_int_t
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


ngx_int_t
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

