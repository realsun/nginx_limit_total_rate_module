/*
 * Author: realsun
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t
ngx_http_limit_traffic_rate_filter_handler(ngx_http_request_t *r);
static ngx_int_t
ngx_http_limit_traffic_rate_filter_init(ngx_conf_t *cf);
static ngx_int_t
    ngx_http_limit_traffic_rate_body_filter(ngx_http_request_t *r, ngx_chain_t *in);
static void* ngx_http_limit_traffic_rate_filter_create_loc_conf(ngx_conf_t *cf);

static char* ngx_http_limit_traffic_rate_filter_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t
ngx_http_limit_traffic_rate_filter_init_zone(ngx_shm_zone_t *shm_zone, void *data);
static void
ngx_http_limit_traffic_rate_filter_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void
ngx_http_limit_traffic_rate_filter_cleanup(void *data);
static char *
ngx_http_limit_traffic_rate_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *
ngx_http_limit_traffic_rate(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t
ngx_http_ip_traffic_not_be_limited(ngx_http_variable_value_t *remote_addr,
    unsigned int min_intranet_ip, unsigned int max_intranet_ip);
static char *
ngx_http_limit_download_intranet_ip(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

typedef struct {
    ngx_queue_t         rq;
    ngx_http_request_t *r;
    off_t               sent;
    ngx_msec_t          start_msec;
    ngx_pid_t           processid;
    ngx_flag_t          skip_limit;
    ngx_uint_t          delay_count;
    ngx_msec_t          skip_start_msec;
}ngx_http_limit_traffic_rate_filter_request_queue_t;

typedef struct {
    ngx_shm_zone_t     *shm_zone;
    ngx_rbtree_node_t  *node;
} ngx_http_limit_traffic_rate_filter_cleanup_t;

typedef struct {
    u_char              color;
    u_short             len;
    u_short             conn;
    ngx_queue_t         rq_top;
    u_char              data[1];
} ngx_http_limit_traffic_rate_filter_node_t;

typedef struct {
    ngx_rbtree_t       *rbtree;
    ngx_int_t           index;
    ngx_int_t           remote_addr_index;
    ngx_str_t           var;
} ngx_http_limit_traffic_rate_filter_ctx_t;

typedef struct {
    size_t              limit_traffic_rate;
    ngx_shm_zone_t     *shm_zone;
    ngx_uint_t          download_min_intranet_ip;
    ngx_uint_t          download_max_intranet_ip;
    size_t              download_min_limit_rate;
    ngx_flag_t          download_limit_rate_module_switch;
    ngx_uint_t          download_limit_max_limit_count;
    ngx_uint_t          download_limit_skip_msec;
} ngx_http_limit_traffic_rate_filter_conf_t;

static ngx_command_t  ngx_http_limit_traffic_rate_filter_commands[] = {

    { ngx_string("limit_traffic_rate_zone"),
      NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE3,
      ngx_http_limit_traffic_rate_zone,
      0,
      0,
      NULL },

    { ngx_string("limit_traffic_rate"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_CONF_TAKE2,
      ngx_http_limit_traffic_rate,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_download_rate_intranet_ip"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_CONF_TAKE2,
      ngx_http_limit_download_intranet_ip,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_download_rate_min"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_traffic_rate_filter_conf_t, download_min_limit_rate),
      NULL },

    { ngx_string("limit_download_rate_module_switch"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_traffic_rate_filter_conf_t, download_limit_rate_module_switch),
      NULL },

    { ngx_string("limit_download_rate_max_limit_count"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_traffic_rate_filter_conf_t, download_limit_max_limit_count),
      NULL },

    { ngx_string("limit_download_rate_skip_msec"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_traffic_rate_filter_conf_t, download_limit_skip_msec),
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_limit_traffic_rate_filter_module_ctx = {
    NULL,                          /* preconfiguration */
    ngx_http_limit_traffic_rate_filter_init,                          /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_limit_traffic_rate_filter_create_loc_conf,  /* create location configuration */
    ngx_http_limit_traffic_rate_filter_merge_loc_conf /* merge location configuration */
};

ngx_module_t  ngx_http_limit_traffic_rate_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_traffic_rate_filter_module_ctx, /* module context */
    ngx_http_limit_traffic_rate_filter_commands,   /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

static ngx_int_t
ngx_http_ip_traffic_not_be_limited(ngx_http_variable_value_t *remote_addr,
    unsigned int min_intranet_ip, unsigned int max_intranet_ip) {
    if (remote_addr->len == sizeof(in_addr_t)) {
        /* ipv4 */
        unsigned int remote_ip = ntohl(*((in_addr_t *)(remote_addr->data)));
        if (remote_ip >= min_intranet_ip &&
                remote_ip <= max_intranet_ip) {
            return NGX_OK;
        }
    }
    return NGX_ERROR;
}

static ngx_int_t
ngx_http_limit_traffic_rate_filter_handler(ngx_http_request_t *r)
{
    size_t                          len = 0;
    size_t                          n = 0;
    size_t                          remote_addr_len = 0;
    uint32_t                        hash = 0;
    ngx_int_t                       rc = 0;
    ngx_slab_pool_t                *shpool = NULL;
    ngx_rbtree_node_t              *node = NULL;
    ngx_rbtree_node_t              *sentinel = NULL;
    ngx_pool_cleanup_t             *cln = NULL;
    ngx_http_variable_value_t      *vv = NULL;
    ngx_http_variable_value_t      *remote_addr = NULL;
    ngx_http_limit_traffic_rate_filter_ctx_t      *ctx = NULL;
    ngx_http_limit_traffic_rate_filter_node_t     *lir = NULL;
    ngx_http_limit_traffic_rate_filter_conf_t     *lircf = NULL;
    ngx_http_limit_traffic_rate_filter_cleanup_t  *lircln = NULL;

    lircf = ngx_http_get_module_loc_conf(r, ngx_http_limit_traffic_rate_filter_module);

    if (!lircf->download_limit_rate_module_switch) {
        return NGX_DECLINED;
    }

    if (r->method != NGX_HTTP_GET) {
        return NGX_DECLINED; 
    }

    if (lircf->shm_zone == NULL) {
        return NGX_DECLINED;
    }

    ctx = lircf->shm_zone->data;

    remote_addr = ngx_http_get_indexed_variable(r, ctx->remote_addr_index);

    if (remote_addr == NULL || remote_addr->not_found) {
        return NGX_DECLINED;
    }

    remote_addr_len = remote_addr->len;

    if (remote_addr_len == 0) {
        return NGX_DECLINED;
    }

    if (remote_addr_len > 1024) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "the value of the binary_remote_addr variable "
                "is more than 1024 bytes: \"%v\"",
                remote_addr);
        return NGX_DECLINED;
    }

    if (ngx_http_ip_traffic_not_be_limited(remote_addr,
            lircf->download_min_intranet_ip, lircf->download_max_intranet_ip) == NGX_OK) {
        return NGX_DECLINED;
    }
    
    vv = ngx_http_get_indexed_variable(r, ctx->index);

    if (vv == NULL || vv->not_found) {
        return NGX_DECLINED;
    }

    len = vv->len;

    if (len == 0) {
        return NGX_DECLINED;
    }

    if (len > 1024) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the value of the \"%V\" variable "
                      "is more than 1024 bytes: \"%v\"",
                      &ctx->var, vv);
        return NGX_DECLINED;
    }

    hash = ngx_crc32_short(vv->data, len);

    cln = ngx_pool_cleanup_add(r->pool, sizeof(ngx_http_limit_traffic_rate_filter_cleanup_t));
    if (cln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    shpool = (ngx_slab_pool_t *) lircf->shm_zone->shm.addr;

    ngx_pid_t this_pid = ngx_getpid();

    ngx_shmtx_lock(&shpool->mutex);

    node = ctx->rbtree->root;
    sentinel = ctx->rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        do {
            lir = (ngx_http_limit_traffic_rate_filter_node_t *) &node->color;

            rc = ngx_memn2cmp(vv->data, lir->data, len, (size_t) lir->len);

            if (rc == 0) {
                ngx_http_limit_traffic_rate_filter_request_queue_t  *req;
                req = ngx_slab_alloc_locked(shpool, 
                        sizeof(ngx_http_limit_traffic_rate_filter_request_queue_t));
                if (node == NULL) {
                    ngx_shmtx_unlock(&shpool->mutex);
                    return NGX_HTTP_SERVICE_UNAVAILABLE;
                }
                req->r = r;
                req->start_msec = ngx_current_msec;
                req->processid = this_pid;
                req->sent = 0;
                req->delay_count = 0;
                req->skip_start_msec = ngx_current_msec;
                req->skip_limit = 0;
                ngx_queue_insert_tail(&(lir->rq_top), &req->rq);
                lir->conn++;
                goto done;
            }

            node = (rc < 0) ? node->left : node->right;

        } while (node != sentinel && hash == node->key);

        break;
    }

    n = offsetof(ngx_rbtree_node_t, color)
        + offsetof(ngx_http_limit_traffic_rate_filter_node_t, data)
        + len;

    node = ngx_slab_alloc_locked(shpool, n);
    if (node == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    lir = (ngx_http_limit_traffic_rate_filter_node_t *) &node->color;

    node->key = hash;
    lir->len = (u_short) len;
    lir->conn = 0;
    
    ngx_queue_init(&(lir->rq_top));
    ngx_http_limit_traffic_rate_filter_request_queue_t  *req;
    req = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_limit_traffic_rate_filter_request_queue_t));
    if (node == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }
    req->r = r;
    req->start_msec = ngx_current_msec;
    req->processid = this_pid;
    req->sent = 0;
    req->delay_count = 0;
    req->skip_start_msec = ngx_current_msec;
    req->skip_limit = 0;
    ngx_queue_insert_tail(&(lir->rq_top), &req->rq);
    lir->conn = 1;

    ngx_memcpy(lir->data, vv->data, len);

    ngx_rbtree_insert(ctx->rbtree, node);

done:

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit traffic rate: %08XD %d", node->key, lir->conn);

    ngx_shmtx_unlock(&shpool->mutex);

    cln->handler = ngx_http_limit_traffic_rate_filter_cleanup;
    lircln = cln->data;

    lircln->shm_zone = lircf->shm_zone;
    lircln->node = node;

    return NGX_DECLINED;
}

static ngx_int_t
    ngx_http_limit_traffic_rate_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    size_t                                   len = 0;
    size_t                                   remote_addr_len = 0;
    ngx_msec_t                               msec = 0;
    ngx_msec_t                               this_msec = 0;
    uint32_t                                 hash = 0;
    ngx_int_t                                rc = 0;
    ngx_int_t                                num = 0;
    ngx_int_t                                conns = 0;
    ngx_int_t                                total_rate = 0;
    ngx_slab_pool_t                         *shpool = NULL;
    ngx_rbtree_node_t                       *node = NULL;
    ngx_rbtree_node_t                       *sentinel = NULL;
    ngx_http_variable_value_t               *vv = NULL;
    ngx_http_variable_value_t               *remote_addr = NULL;
    ngx_http_limit_traffic_rate_filter_ctx_t      *ctx = NULL;
    ngx_http_limit_traffic_rate_filter_node_t     *lir = NULL;
    ngx_http_limit_traffic_rate_filter_conf_t     *lircf = NULL;
    
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "limit traffic rate filter");
    
    lircf = ngx_http_get_module_loc_conf(r, ngx_http_limit_traffic_rate_filter_module);

    if (!lircf->download_limit_rate_module_switch) {
        return ngx_http_next_body_filter(r, in);
    }

    if (r->method != NGX_HTTP_GET) {
        return ngx_http_next_body_filter(r, in); 
    }

    if (lircf->shm_zone == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    ctx = lircf->shm_zone->data;

    remote_addr = ngx_http_get_indexed_variable(r, ctx->remote_addr_index);

    if (remote_addr == NULL || remote_addr->not_found) {
        return ngx_http_next_body_filter(r, in);
    }

    remote_addr_len = remote_addr->len;

    if (remote_addr_len == 0) {
        return ngx_http_next_body_filter(r, in);
    }

    if (remote_addr_len > 1024) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "the value of the binary_remote_addr variable "
                "is more than 1024 bytes: \"%v\"",
                remote_addr);
        return ngx_http_next_body_filter(r, in);
    }

    if (ngx_http_ip_traffic_not_be_limited(remote_addr,
            lircf->download_min_intranet_ip, lircf->download_max_intranet_ip) == NGX_OK) {
        return ngx_http_next_body_filter(r, in);
    }

    vv = ngx_http_get_indexed_variable(r, ctx->index);

    if (vv == NULL || vv->not_found) {
        return ngx_http_next_body_filter(r, in);
    }

    len = vv->len;

    if (len == 0) {
        return ngx_http_next_body_filter(r, in);
    }

    if (len > 1024) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the value of the \"%V\" variable "
                      "is more than 1024 bytes: \"%v\"",
                      &ctx->var, vv);
        return ngx_http_next_body_filter(r, in);
    }

    hash = ngx_crc32_short(vv->data, len);

    shpool = (ngx_slab_pool_t *) lircf->shm_zone->shm.addr;

    ngx_pid_t this_pid = ngx_getpid();

    ngx_shmtx_lock(&shpool->mutex);

    node = ctx->rbtree->root;
    sentinel = ctx->rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        do {
            lir = (ngx_http_limit_traffic_rate_filter_node_t *) &node->color;

            rc = ngx_memn2cmp(vv->data, lir->data, len, (size_t) lir->len);

            if (rc == 0) {
                ngx_queue_t *p = lir->rq_top.next;
                ngx_http_limit_traffic_rate_filter_request_queue_t * tr = NULL;
                ngx_http_limit_traffic_rate_filter_request_queue_t * this_tr = NULL;
                total_rate = 0;
                this_msec = 1;
                for (; p;) {
                    tr = ngx_queue_data(p, ngx_http_limit_traffic_rate_filter_request_queue_t, rq); 
                    if(tr->r == r && tr->processid == this_pid) {
                        tr->sent = r->connection->sent;
                        this_msec = (ngx_msec_t)(ngx_current_msec - tr->start_msec);
                        this_msec = this_msec > 100 ? this_msec : 100;
                        this_tr = tr;
                    }
                    msec = (ngx_msec_t)(ngx_current_msec - tr->start_msec);
                    msec = msec > 1000 ? msec : 1000;
                    total_rate += (tr->sent / msec);
                    conns++;

                    if (ngx_queue_last(&lir->rq_top) == p) {
                        break;
                    }
                    p = ngx_queue_next(p);
                }

                conns = conns > 0 ? conns : 1;
                num = lircf->limit_traffic_rate - 1000 * total_rate;
                num = num / conns + 1000 * r->connection->sent / this_msec;
                if (r->connection->sent == 0) {
                    num = 3 * num / conns;
                }

                num = ((size_t)num > lircf->download_min_limit_rate) ? 
                    num : (ngx_int_t)lircf->download_min_limit_rate;
                num = ((size_t)num >lircf->limit_traffic_rate) ? 
                    (ngx_int_t)lircf->limit_traffic_rate : num;

                if (this_tr == NULL) {
                    goto done;
                } 
                if (this_tr->skip_limit) {

                    if ((ngx_msec_t)(ngx_current_msec - this_tr->skip_start_msec) 
                            >= (ngx_msec_t)lircf->download_limit_skip_msec) {
                        this_tr->skip_limit = 0;
                    } else {
                        r->limit_rate = lircf->limit_traffic_rate;
                        goto done;
                    }
                } else {
                    if (this_tr->delay_count >= lircf->download_limit_max_limit_count) {
                        this_tr->skip_limit = 1;
                        this_tr->delay_count = 0;
                        this_tr->skip_start_msec = ngx_current_msec;
                        r->limit_rate = lircf->limit_traffic_rate;
                        goto done;
                    } 
                    if ((ngx_uint_t)num <= lircf->download_min_limit_rate) {
                        this_tr->delay_count++;
                    }
                }

                r->limit_rate = num;

                ngx_log_debug8(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                        "download limit lircf->limit_traffic_rate[%d], "
                        "this_msec[%d], r->limit_rate[%d], "
                        "this_tr->skip_limit[%d], lir->conns[%d], "
                        "conns[%d], "
                        "this_rate[%d] total_rate[%d]",
                        lircf->limit_traffic_rate,
                        this_msec,
                        r->limit_rate,
                        this_tr->skip_limit,
                        lir->conn,
                        conns,
                        r->connection->sent / this_msec,
                        total_rate);

                goto done;
            }

            node = (rc < 0) ? node->left : node->right;

        } while (node != sentinel && hash == node->key);

        break;
    }
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "rbtree search fail: %08XD %d", node->key, r->limit_rate);
    
done:

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit traffic rate: %08XD %d", node->key, r->limit_rate);

    ngx_shmtx_unlock(&shpool->mutex);

    return ngx_http_next_body_filter(r, in);
}

static ngx_int_t
ngx_http_limit_traffic_rate_filter_log_handler(ngx_http_request_t *r)
{
    size_t                          len = 0;
    size_t                          remote_addr_len = 0;
    uint32_t                        hash = 0;
    ngx_int_t                       rc = 0;
    ngx_slab_pool_t                *shpool = NULL;
    ngx_rbtree_node_t              *node = NULL;
    ngx_rbtree_node_t              *sentinel = NULL;
    ngx_http_variable_value_t      *vv = NULL;
    ngx_http_variable_value_t      *remote_addr = NULL;
    ngx_http_limit_traffic_rate_filter_ctx_t      *ctx = NULL;
    ngx_http_limit_traffic_rate_filter_node_t     *lir = NULL;
    ngx_http_limit_traffic_rate_filter_conf_t     *lircf = NULL;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                            "limit traffic rate log phase handler");
    
    lircf = ngx_http_get_module_loc_conf(r, ngx_http_limit_traffic_rate_filter_module);

    if (!lircf->download_limit_rate_module_switch) {
        return NGX_DECLINED;
    }

    if (lircf->shm_zone == NULL) {
        return NGX_DECLINED;
    }

    ctx = lircf->shm_zone->data;

    remote_addr = ngx_http_get_indexed_variable(r, ctx->remote_addr_index);

    if (remote_addr == NULL || remote_addr->not_found) {
        return NGX_DECLINED;
    }

    remote_addr_len = remote_addr->len;

    if (remote_addr_len == 0) {
        return NGX_DECLINED;
    }

    if (remote_addr_len > 1024) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "the value of the binary_remote_addr variable "
                "is more than 1024 bytes: \"%v\"",
                remote_addr);
        return NGX_DECLINED;
    }

    if (ngx_http_ip_traffic_not_be_limited(remote_addr, 
            lircf->download_min_intranet_ip, lircf->download_max_intranet_ip) == NGX_OK) {
        return NGX_DECLINED;
    }

    vv = ngx_http_get_indexed_variable(r, ctx->index);

    if (vv == NULL || vv->not_found) {
        return NGX_DECLINED;
    }

    len = vv->len;

    if (len == 0) {
        return NGX_DECLINED;
    }

    if (len > 1024) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the value of the \"%V\" variable "
                      "is more than 1024 bytes: \"%v\"",
                      &ctx->var, vv);
        return NGX_DECLINED;
    }

    hash = ngx_crc32_short(vv->data, len);

    shpool = (ngx_slab_pool_t *) lircf->shm_zone->shm.addr;

    ngx_pid_t this_pid = ngx_getpid();

    ngx_shmtx_lock(&shpool->mutex);

    node = ctx->rbtree->root;
    sentinel = ctx->rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        do {
            lir = (ngx_http_limit_traffic_rate_filter_node_t *) &node->color;

            rc = ngx_memn2cmp(vv->data, lir->data, len, (size_t) lir->len);

            if (rc == 0) {
                ngx_queue_t *p = lir->rq_top.next;
                ngx_http_limit_traffic_rate_filter_request_queue_t * tr;
                for (; p;) {
                    tr = ngx_queue_data(p, ngx_http_limit_traffic_rate_filter_request_queue_t, rq); 
                    if (tr->r == r && tr->processid == this_pid) {
                        tr->r = NULL;
                        ngx_queue_remove(p);
                        ngx_slab_free_locked(shpool, tr);
                        goto done;
                    }

                    if(ngx_queue_last(&lir->rq_top) == p){
                        break;
                    }
                    p = ngx_queue_next(p);
                }
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "queue search fail: %08XD", node->key);
            }

            node = (rc < 0) ? node->left : node->right;

        } while (node != sentinel && hash == node->key);

        break;
    }
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "rbtree search fail: %08XD %d", node->key, r->limit_rate);
    
done:

    ngx_shmtx_unlock(&shpool->mutex);

    return NGX_DECLINED;
}

static ngx_int_t
ngx_http_limit_traffic_rate_filter_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h = NULL;
    ngx_http_core_main_conf_t  *cmcf = NULL;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_traffic_rate_filter_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_traffic_rate_filter_log_handler;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_limit_traffic_rate_body_filter;

    return NGX_OK;
}

static void *
ngx_http_limit_traffic_rate_filter_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_limit_traffic_rate_filter_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_traffic_rate_filter_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    conf->shm_zone = NULL;
    conf->limit_traffic_rate = NGX_CONF_UNSET_SIZE;
    conf->download_min_limit_rate = NGX_CONF_UNSET_SIZE;
    conf->download_limit_rate_module_switch = NGX_CONF_UNSET;
    conf->download_min_intranet_ip = NGX_CONF_UNSET_UINT;
    conf->download_max_intranet_ip = NGX_CONF_UNSET_UINT;
    conf->download_limit_max_limit_count = NGX_CONF_UNSET_UINT;
    conf->download_limit_skip_msec = NGX_CONF_UNSET_UINT;
    return conf;
}

static char *
ngx_http_limit_traffic_rate_filter_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_traffic_rate_filter_conf_t *prev = parent;
    ngx_http_limit_traffic_rate_filter_conf_t *conf = child;

    ngx_conf_merge_size_value(conf->limit_traffic_rate, prev->limit_traffic_rate, 0);
    ngx_conf_merge_size_value(conf->download_min_limit_rate, prev->download_min_limit_rate, 10240);
    ngx_conf_merge_value(conf->download_limit_rate_module_switch, 
                         prev->download_limit_rate_module_switch, 0);
   
    ngx_conf_merge_uint_value(conf->download_min_intranet_ip, prev->download_min_intranet_ip, 0);
    ngx_conf_merge_uint_value(conf->download_max_intranet_ip, prev->download_max_intranet_ip, 0);
    ngx_conf_merge_uint_value(conf->download_limit_max_limit_count,
                        prev->download_limit_max_limit_count, 5);
    ngx_conf_merge_uint_value(conf->download_limit_skip_msec,
                        prev->download_limit_skip_msec, 500);
    
    if (conf->shm_zone == NULL) {
        conf->shm_zone = prev->shm_zone;
    }

    return NGX_CONF_OK;
}

static void
ngx_http_limit_traffic_rate_filter_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t           **p = NULL;
    ngx_http_limit_traffic_rate_filter_node_t   *lirn = NULL;
    ngx_http_limit_traffic_rate_filter_node_t   *lirnt = NULL;

    for (;;) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lirn = (ngx_http_limit_traffic_rate_filter_node_t *) &node->color;
            lirnt = (ngx_http_limit_traffic_rate_filter_node_t *) &temp->color;

            p = (ngx_memn2cmp(lirn->data, lirnt->data, lirn->len, lirnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

static ngx_int_t
ngx_http_limit_traffic_rate_filter_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_limit_traffic_rate_filter_ctx_t  *octx = data;

    size_t                      len = 0;
    ngx_slab_pool_t            *shpool = NULL;
    ngx_rbtree_node_t          *sentinel = NULL;
    ngx_http_limit_traffic_rate_filter_ctx_t  *ctx = NULL;

    ctx = shm_zone->data;

    if (octx) {
        if (ngx_strcmp(ctx->var.data, octx->var.data) != 0) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "limit_traffic_rate_filter \"%V\" uses the \"%V\" variable "
                          "while previously it used the \"%V\" variable",
                          &shm_zone->shm.name, &ctx->var, &octx->var);
            return NGX_ERROR;
        }

        ctx->rbtree = octx->rbtree;

        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->rbtree = shpool->data;

        return NGX_OK;
    }

    ctx->rbtree = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_t));
    if (ctx->rbtree == NULL) {
        return NGX_ERROR;
    }

    shpool->data = ctx->rbtree;

    sentinel = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_node_t));
    if (sentinel == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(ctx->rbtree, sentinel,
                    ngx_http_limit_traffic_rate_filter_rbtree_insert_value);

    len = sizeof(" in limit_traffic_rate_filter \"\"") + shm_zone->shm.name.len;

    shpool->log_ctx = ngx_slab_alloc(shpool, len);
    if (shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(shpool->log_ctx, " in limit_traffic_rate_filter \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}

static void
ngx_http_limit_traffic_rate_filter_cleanup(void *data)
{
    ngx_http_limit_traffic_rate_filter_cleanup_t  *lircln = data;

    ngx_slab_pool_t             *shpool = NULL;
    ngx_rbtree_node_t           *node = NULL;
    ngx_http_limit_traffic_rate_filter_ctx_t   *ctx = NULL;
    ngx_http_limit_traffic_rate_filter_node_t  *lir = NULL;

    ctx = lircln->shm_zone->data;
    shpool = (ngx_slab_pool_t *) lircln->shm_zone->shm.addr;
    node = lircln->node;
    lir = (ngx_http_limit_traffic_rate_filter_node_t *) &node->color;

    ngx_shmtx_lock(&shpool->mutex);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, lircln->shm_zone->shm.log, 0,
                   "limit traffic rate cleanup: %08XD %d", node->key, lir->conn);

    lir->conn--;

    if (lir->conn == 0) {        
        ngx_queue_t *p = lir->rq_top.next;
        ngx_queue_t *c = NULL;
        ngx_http_limit_traffic_rate_filter_request_queue_t *tr = NULL;
        for(; p;) {
            c = p;
            p = ngx_queue_next(p);

            if(ngx_queue_next(c) && ngx_queue_prev(c)){
                ngx_queue_remove(c);
            }
            tr = ngx_queue_data(c, ngx_http_limit_traffic_rate_filter_request_queue_t, rq); 
            if (!tr->r){
                ngx_slab_free_locked(shpool, tr);
            }

            if(ngx_queue_last(&lir->rq_top) == p){
                break;
            }
        }

        ngx_rbtree_delete(ctx->rbtree, node);
        ngx_slab_free_locked(shpool, node);
    }

    ngx_shmtx_unlock(&shpool->mutex);
}

static char *
ngx_http_limit_traffic_rate_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t                     n = 0;
    ngx_str_t                  *value = NULL;
    ngx_str_t                   binary_remote_str;
    ngx_shm_zone_t             *shm_zone = NULL;
    ngx_http_limit_traffic_rate_filter_ctx_t  *ctx = NULL;

    value = cf->args->elts;

    if (value[2].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    value[2].len--;
    value[2].data++;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_traffic_rate_filter_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->index = ngx_http_get_variable_index(cf, &value[2]);
    if (ctx->index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    ctx->var = value[2];

    ngx_str_set(&binary_remote_str, "binary_remote_addr");
    ctx->remote_addr_index = ngx_http_get_variable_index(cf, &binary_remote_str);
    if (ctx->remote_addr_index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    n = ngx_parse_size(&value[3]);

    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid size of limit_traffic_rate_zone \"%V\"", &value[3]);
        return NGX_CONF_ERROR;
    }

    if (n < (ngx_int_t) (8 * ngx_pagesize)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "limit_traffic_rate_zone \"%V\" is too small", &value[1]);
        return NGX_CONF_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, &value[1], n,
                                     &ngx_http_limit_traffic_rate_filter_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "limit_traffic_rate_zone \"%V\" is already bound to variable \"%V\"",
                        &value[1], &ctx->var);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_limit_traffic_rate_filter_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}

static char *
ngx_http_limit_traffic_rate(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_limit_traffic_rate_filter_conf_t  *lircf = conf;

    ngx_int_t   n = 0;
    ngx_str_t  *value = NULL;

    if (lircf->shm_zone) {
        return "is duplicate";
    }

    value = cf->args->elts;

    lircf->shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                           &ngx_http_limit_traffic_rate_filter_module);
    if (lircf->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    n = ngx_parse_size(&value[2]);
    
    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid size of limit_traffic_rate \"%V\"", &value[3]);
        return NGX_CONF_ERROR;
    }

    if (n <= 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid size of limit_traffic_rate \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    lircf->limit_traffic_rate = n;

    return NGX_CONF_OK;
}

static char *
ngx_http_limit_download_intranet_ip(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf) {
    ngx_http_limit_traffic_rate_filter_conf_t *lucf = conf;

    ngx_str_t  *min_ip_str = NULL;
    ngx_str_t  *max_ip_str = NULL;
    ngx_str_t  *value = NULL;

    value = cf->args->elts;
    min_ip_str = &value[1];
    max_ip_str = &value[2];

    lucf->download_min_intranet_ip = ntohl(ngx_inet_addr(min_ip_str->data, min_ip_str->len));
    lucf->download_max_intranet_ip = ntohl(ngx_inet_addr(max_ip_str->data, max_ip_str->len));

    return NGX_CONF_OK;
}

