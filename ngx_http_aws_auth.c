#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/buffer.h>

#include "aws_functions.h"

#define AWS_S3_VARIABLE "s3_auth_token"
#define AWS_DATE_VARIABLE "aws_date"

static void
*ngx_http_aws_auth_create_loc_conf(ngx_conf_t *cf);

static char
*ngx_http_aws_auth_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static
ngx_int_t ngx_aws_auth_req_init(ngx_conf_t *cf);

static char
*ngx_http_aws_endpoint(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char
*ngx_http_aws_sign(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_command_t ngx_http_aws_auth_commands[] = {
        {ngx_string("aws_access_key"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_aws_auth_conf_t, access_key),
         NULL},

        {ngx_string("aws_secret_key"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_aws_auth_conf_t, secret_key),
         NULL},

        {ngx_string("aws_region"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_aws_auth_conf_t, region),
         NULL},

        {ngx_string("aws_endpoint"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
         ngx_http_aws_endpoint,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_aws_auth_conf_t, endpoint),
         NULL},

        {ngx_string("aws_service"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_aws_auth_conf_t, service),
         NULL},

        {ngx_string("aws_s3_bucket"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_aws_auth_conf_t, bucket_name),
         NULL},

        {ngx_string("aws_sign"),
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
         ngx_http_aws_sign,
         0,
         0,
         NULL},

        ngx_null_command
};

static ngx_http_module_t ngx_http_aws_auth_module_ctx = {
        NULL,                     /* preconfiguration */
        ngx_aws_auth_req_init,                                  /* postconfiguration */

        NULL,                                  /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        ngx_http_aws_auth_create_loc_conf,     /* create location configuration */
        ngx_http_aws_auth_merge_loc_conf       /* merge location configuration */
};


ngx_module_t ngx_http_aws_auth_module = {
        NGX_MODULE_V1,
        &ngx_http_aws_auth_module_ctx,              /* module context */
        ngx_http_aws_auth_commands,                 /* module directives */
        NGX_HTTP_MODULE,                       /* module type */
        NULL,                                  /* init master */
        NULL,                                  /* init module */
        NULL,                                  /* init process */
        NULL,                                  /* init thread */
        NULL,                                  /* exit thread */
        NULL,                                  /* exit process */
        NULL,                                  /* exit master */
        NGX_MODULE_V1_PADDING
};


static void *
ngx_http_aws_auth_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_aws_auth_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_aws_auth_conf_t));
    conf->enabled = 0;
    ngx_str_set(&conf->endpoint, "s3.amazonaws.com");
    ngx_str_set(&conf->service, "s3");

    if (conf == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "Error in ngx_aws_auth configuration");
        return NGX_CONF_ERROR;
    }

    return conf;
}

static char *
ngx_http_aws_auth_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_aws_auth_conf_t *prev = parent;
    ngx_http_aws_auth_conf_t *conf = child;


    if (conf->enabled) {
        ngx_conf_merge_str_value(conf->access_key, prev->access_key, "");
        ngx_conf_merge_str_value(conf->secret_key, prev->secret_key, "");
        ngx_conf_merge_str_value(conf->region, prev->region, "");
        ngx_conf_merge_str_value(conf->service, prev->service, "s3");
        ngx_conf_merge_str_value(conf->endpoint, prev->endpoint, "s3.amazonaws.com");
        ngx_conf_merge_str_value(conf->bucket_name, prev->bucket_name, "");

        ngx_uint_t config_invalid = 0;
        if (conf->access_key.len == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "Error aws_access_key key missing");
            config_invalid = 1;
        }

        if (conf->region.len == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "Error aws_region missing");
            config_invalid = 1;
        }

        if (conf->bucket_name.len == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "Error aws_s3_bucket missing");
            config_invalid = 1;
        }

        if (conf->secret_key.len == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "Error aws_secret_key missing");
            config_invalid = 1;
        }

        if (config_invalid) {
            return NGX_CONF_ERROR;
        }

        time_t rawtime;
        time(&rawtime);
        update_key_signature(cf->pool, conf, &rawtime);
    }
    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_aws_proxy_sign(ngx_http_request_t *r) {
    ngx_http_aws_auth_conf_t *conf = ngx_http_get_module_loc_conf(r, ngx_http_aws_auth_module);
    if (!conf->enabled) {
        /* return directly if module is not enabled */
        return NGX_DECLINED;
    }
    ngx_table_elt_t *h;
    header_pair_t *hv;

    if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
        /* We do not wish to support anything with a body as signing for a body is unimplemented */
        return NGX_HTTP_NOT_ALLOWED;
    }

    update_key_signature(r->pool, conf, &r->start_sec);

    const ngx_array_t *headers_out = ngx_aws_auth__sign(
            r->pool, r,
            &conf->access_key, &conf->signing_key_decoded, &conf->key_scope,
            &conf->bucket_name, &conf->endpoint);

    ngx_uint_t i;
    for (i = 0; i < headers_out->nelts; i++) {
        hv = (header_pair_t *) ((u_char *) headers_out->elts + headers_out->size * i);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "header name %s, value %s", hv->key.data, hv->value.data);

        if (ngx_strncmp(hv->key.data, HOST_HEADER.data, hv->key.len) == 0) {
            /* host header is controlled by proxy pass directive and hence
               cannot be set by our module */
            continue;
        }

        h = ngx_list_push(&r->headers_in.headers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        h->hash = 1;
        h->key = hv->key;
        h->lowcase_key = hv->key.data; /* We ensure that header names are already lowercased */
        h->value = hv->value;
    }
    return NGX_OK;
}

static char *
ngx_http_aws_endpoint(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    char *p = conf;

    ngx_str_t *field, *value;

    field = (ngx_str_t * )(p + cmd->offset);

    value = cf->args->elts;

    *field = value[1];

    return NGX_CONF_OK;
}

static char *
ngx_http_aws_sign(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_aws_auth_conf_t *mconf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_aws_auth_module);
    mconf->enabled = 1;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_aws_auth_req_init(ngx_conf_t *cf) {
    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "Missing NGX HTTP ACCESS PHASE handlers");
        return NGX_ERROR;
    }

    *h = ngx_http_aws_proxy_sign;

    return NGX_OK;
}