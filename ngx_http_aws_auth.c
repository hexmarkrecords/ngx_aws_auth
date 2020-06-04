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
#define AWS_DATE_WIDTH 8

static void *ngx_http_aws_auth_create_loc_conf(ngx_conf_t *cf);

static char *ngx_http_aws_auth_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_int_t ngx_aws_auth_req_init(ngx_conf_t *cf);

static char *ngx_http_aws_endpoint(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_http_aws_sign(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

typedef struct {
    ngx_str_t access_key;
    ngx_str_t key_scope;
    ngx_str_t secret_key;
    ngx_str_t signing_key_decoded;
    ngx_str_t region;
    ngx_str_t service;
    ngx_str_t endpoint;
    ngx_str_t bucket_name;
    ngx_uint_t enabled;
} ngx_http_aws_auth_conf_t;


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

static void
update_credentials(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf, time_t *time_p);

static uint8_t *
sign(ngx_pool_t *pool, uint8_t *key, uint8_t *val);

static void
init_config_field(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf);

static void
init_signing_key_decoded(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf);

static void
update_key_scope(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf, uint8_t *dateStamp);

static void
update_signing_key_decoded(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf, uint8_t *dateStamp);

static int
is_signing_credentials_valid(ngx_http_aws_auth_conf_t *conf, ngx_str_t *dateTimeStamp);


static void *
ngx_http_aws_auth_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_aws_auth_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_aws_auth_conf_t));
    conf->enabled = 0;
    ngx_str_set(&conf->endpoint, "s3.amazonaws.com");
    ngx_str_set(&conf->service, "s3");

    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    return conf;
}


static void
init_config_field(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf)
{
    if (conf->key_scope.data == NULL) {
        conf->key_scope.data = ngx_pcalloc(pool, 100);
    }
}


static void
init_signing_key_decoded(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf)
{
    if (conf->signing_key_decoded.data == NULL) {
        conf->signing_key_decoded.data = ngx_pcalloc(pool, 100);
    }
}


static void
update_key_scope(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf, uint8_t *dateStamp)
{
    // Update Key Scope
    int keyScopeLength = strlen((char *) conf->region.data) + strlen((char *) conf->service.data) + 16;
    uint8_t *keyScopeBuffer = ngx_pcalloc(pool, keyScopeLength * sizeof(uint8_t));

    ngx_memcpy(keyScopeBuffer, dateStamp, AWS_DATE_WIDTH);
    sprintf(&((char *) keyScopeBuffer)[AWS_DATE_WIDTH], "/%s/%s/aws4_request", conf->region.data, conf->service.data);

    conf->key_scope.len = ngx_strlen(keyScopeBuffer);
    ngx_memcpy(conf->key_scope.data, keyScopeBuffer, conf->key_scope.len);
}


static void
update_signing_key_decoded(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf, uint8_t *dateStamp)
{
    // Update Signature Key
    // extra byte for null char
    size_t signature_key_buffer_length = (strlen((char *) conf->secret_key.data) + 5) * sizeof(uint8_t);
    uint8_t *signature_key_buffer = ngx_pcalloc(pool, signature_key_buffer_length);

    sprintf((char *) signature_key_buffer, "AWS4%s", conf->secret_key.data);
    uint8_t *kDate = sign(pool, signature_key_buffer, (uint8_t *) dateStamp);
    uint8_t *kRegion = sign(pool, kDate, conf->region.data);
    uint8_t *kService = sign(pool, kRegion, conf->service.data);
    uint8_t *kSigning = sign(pool, kService, (uint8_t *) "aws4_request");

    conf->signing_key_decoded.len = ngx_strlen(kSigning);
    ngx_memcpy(conf->signing_key_decoded.data, kSigning, conf->signing_key_decoded.len);
}


static int
is_signing_credentials_valid(ngx_http_aws_auth_conf_t *conf, ngx_str_t *dateTimeStamp)
{
    return !(conf->key_scope.len == 0
             || !!ngx_strncmp((char *) &conf->key_scope.data, (char *) &dateTimeStamp->data, AWS_DATE_WIDTH));
}


static void
update_credentials(ngx_pool_t *pool, ngx_http_aws_auth_conf_t *conf, time_t *time_p)
{
    init_key_scope(pool, conf);
    init_signing_key_decoded(pool, conf);

    const ngx_str_t *dateTimeStamp = ngx_aws_auth__compute_request_time(pool, time_p);

    if (!is_signing_credentials_valid(conf, dateTimeStamp)) {

        uint8_t *dateStamp = ngx_pcalloc(pool, (AWS_DATE_WIDTH + 1) * sizeof(uint8_t));
        ngx_memcpy(dateStamp, dateTimeStamp->data, AWS_DATE_WIDTH);

        update_key_scope(pool, conf, dateStamp);
        update_signing_key_decoded(pool, conf, dateStamp);
    }
}


static char *
ngx_http_aws_auth_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_aws_auth_conf_t *prev = parent;
    ngx_http_aws_auth_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->access_key, prev->access_key, "");
    ngx_conf_merge_str_value(conf->secret_key, prev->secret_key, "");
    ngx_conf_merge_str_value(conf->region, prev->region, "");
    ngx_conf_merge_str_value(conf->service, prev->service, "s3");
    ngx_conf_merge_str_value(conf->endpoint, prev->endpoint, "s3.amazonaws.com");
    ngx_conf_merge_str_value(conf->bucket_name, prev->bucket_name, "");

    if (conf->secret_key.len == 0) {
        return NGX_CONF_ERROR;
    }

    time_t rawtime;
    time(&rawtime);
    return update_credentials(cf->pool, conf, &rawtime);
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

    update_credentials(r->pool, conf, &r->start_sec);

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
        return NGX_ERROR;
    }

    *h = ngx_http_aws_proxy_sign;

    return NGX_OK;
}


static
uint8_t *sign(ngx_pool_t *pool, uint8_t *key, uint8_t *val) {
    unsigned int len = 64;

    uint8_t *hash = ngx_pcalloc(pool, len * sizeof(uint8_t));

    HMAC_CTX hmac;
    HMAC_CTX_init(&hmac);
    HMAC_Init(&hmac, key, strlen((char *) key), EVP_sha256());
    HMAC_Update(&hmac, val, strlen((char *) val));
    HMAC_Final(&hmac, hash, &len);
    HMAC_CTX_cleanup(&hmac);

    return hash;
}
