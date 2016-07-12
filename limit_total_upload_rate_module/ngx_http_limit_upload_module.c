/*
 * Author: realsun
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_msec_t                start_msec;
    off_t                     read;
    ngx_http_request_t       *r;
    ngx_queue_t               rq;
    ngx_pid_t                 processid;
    ngx_uint_t                delay_count;
    char                      skip_limit;
    ngx_msec_t                delay_msec;
} ngx_http_limit_upload_node_t;

typedef struct {
    u_short                   conn;
    ngx_queue_t              *queue;
} ngx_http_limit_upload_shm_queue_t;

typedef struct {
    size_t                    limit_rate_after;
    ngx_shm_zone_t           *shm_zone;
    size_t                    limit_rate_period[24];
    ngx_int_t                 remote_addr_index;
    ngx_uint_t                upload_min_intranet_ip;
    ngx_uint_t                upload_max_intranet_ip;
    size_t                    upload_min_limit_rate;
    ngx_flag_t                upload_limit_rate_module_switch;
    ngx_uint_t                upload_limit_max_limit_count;
    ngx_uint_t                upload_limit_skip_msec;
    ngx_uint_t                upload_max_delay_time;
} ngx_http_limit_upload_conf_t;

typedef struct {
    off_t                     received;
    ngx_http_event_handler_pt read_event_handler;
    ngx_http_event_handler_pt write_event_handler;
} ngx_http_limit_upload_ctx_t;

typedef struct {
    ngx_shm_zone_t     *shm_zone;
    ngx_http_request_t *r;
    ngx_pid_t           processid;
} ngx_http_limit_upload_cleanup_t;

static ngx_int_t ngx_http_limit_upload_init(ngx_conf_t *cf);
static void *ngx_http_limit_upload_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_limit_upload_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);

static ngx_int_t ngx_http_limit_upload_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_limit_upload_input_body_filter(ngx_http_request_t *r,
    ngx_buf_t *buf);

static void ngx_http_limit_upload_delay(ngx_http_request_t *r);

static char *ngx_http_limit_upload_rate_period(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf);
static void ngx_http_get_current_interval_limit_rate(
    ngx_http_limit_upload_conf_t *llcf, time_t curtime, size_t *cur_limit_rate);
static ngx_int_t
ngx_http_ip_upload_not_be_limited(ngx_http_variable_value_t *remote_addr,
    unsigned int min_intranet_ip, unsigned int max_intranet_ip);
static char *
ngx_http_limit_upload_intranet_ip(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf);
static char *
ngx_http_limit_upload_rate_shm_zone(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static void
ngx_http_limit_upload_cleanup(void *data);

static ngx_command_t ngx_http_limit_upload_commands[] = {
    { ngx_string("limit_upload_rate_after"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, limit_rate_after),
      NULL },

    { ngx_string("limit_upload_rate_period"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_CONF_TAKE1,
      ngx_http_limit_upload_rate_period,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_upload_rate_intranet_ip"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_CONF_TAKE2,
      ngx_http_limit_upload_intranet_ip,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_upload_rate_min"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF 
                         | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, upload_min_limit_rate),
      NULL },

    { ngx_string("upload_limit_rate_module_switch"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, upload_limit_rate_module_switch),
      NULL },

    { ngx_string("limit_upload_rate_shm_zone"),
      NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE2,
      ngx_http_limit_upload_rate_shm_zone,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_upload_max_limit_count"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, upload_limit_max_limit_count),
      NULL },

    { ngx_string("limit_upload_skip_msec"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, upload_limit_skip_msec),
      NULL },

    { ngx_string("limit_upload_max_delay_time"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF
                         | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, upload_max_delay_time),
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_http_limit_upload_module_ctx = {
    NULL,                                     /* preconfiguration */
    ngx_http_limit_upload_init,               /* postconfiguration */

    NULL,                                     /* create main configuration */
    NULL,                                     /* init main configuration */

    NULL,                                     /* create server configuration */
    NULL,                                     /* merge server configuration */

    ngx_http_limit_upload_create_loc_conf,    /* create location configration */
    ngx_http_limit_upload_merge_loc_conf      /* merge location configration */
};

ngx_module_t ngx_http_limit_upload_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_upload_module_ctx,        /* module context */
    ngx_http_limit_upload_commands,           /* module directives */
    NGX_HTTP_MODULE,                          /* module type */
    NULL,                                     /* init master */
    NULL,                                     /* init module */
    NULL,                                     /* init process */
    NULL,                                     /* init thread */
    NULL,                                     /* exit thread */
    NULL,                                     /* exit process */
    NULL,                                     /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_input_body_filter_pt  ngx_http_next_input_body_filter;

static void ngx_http_get_current_interval_limit_rate(
    ngx_http_limit_upload_conf_t *llcf, time_t curtime, size_t *cur_limit_rate) {
    // 校对时区
    time_t tmp_time = curtime + (7 * 3600);
    struct tm usual_time;
    if (NULL == gmtime_r(&tmp_time, &usual_time)) {
        *cur_limit_rate = llcf->limit_rate_period[0];
        return;
    }
    *cur_limit_rate = llcf->limit_rate_period[usual_time.tm_hour];
    return;
}

static ngx_int_t
ngx_http_ip_upload_not_be_limited(ngx_http_variable_value_t *remote_addr,
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
ngx_http_limit_upload_input_body_filter(ngx_http_request_t *r, ngx_buf_t *buf)
{
    off_t                          excess = 0;
    ngx_int_t                      rc = NGX_OK;
    ngx_int_t                      total_rate = 0;
    ngx_int_t                      num = 0;
    ngx_int_t                      this_rate = 0;
    ngx_msec_t                     delay = 0;
    ngx_http_core_loc_conf_t      *clcf = NULL;
    ngx_http_limit_upload_ctx_t   *ctx = NULL;
    ngx_http_limit_upload_conf_t  *llcf = NULL;
    ngx_time_t                    *current_time = NULL;
    ngx_msec_t                     msec = 0;
    ngx_msec_t                     this_msec = 0;
    ngx_msec_t                     curtime_msec = 0;
    size_t                         cur_limit_rate = 0;
    size_t                         remote_addr_len = 0;
    ngx_shm_zone_t                *shm_zone = NULL;
    ngx_slab_pool_t               *shpool = NULL;
    ngx_http_limit_upload_shm_queue_t   *shm_queue = NULL;
    ngx_queue_t                         *tmp_queue = NULL;
    ngx_http_limit_upload_node_t        *tmp_node = NULL;
    u_short                              count = 0;
    ngx_http_variable_value_t           *remote_addr = NULL;
    ngx_int_t                            conns = 0;


    rc = ngx_http_next_input_body_filter(r, buf);
    if (rc != NGX_OK) {
        return rc;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_limit_upload_module);

    llcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_upload_module);

    if (!llcf->upload_limit_rate_module_switch) {
        return NGX_OK;
    }

    if (r->method != NGX_HTTP_POST && r->method != NGX_HTTP_PUT) {
        return NGX_OK;
    }

    //input 0
    if (ngx_buf_size(buf) == 0) {
        return NGX_OK; 
    }
    
    // left 0
    if (r->request_body->rest == 0) {
        return NGX_OK;
    }

    // 判断是否上传限速
    remote_addr = ngx_http_get_indexed_variable(r, llcf->remote_addr_index);
    
    if (remote_addr == NULL || remote_addr->not_found) {
        return NGX_OK;
    }

    remote_addr_len = remote_addr->len;

    if (remote_addr_len == 0) {
        return NGX_OK;
    }

    if (remote_addr_len > 1024) {
        return NGX_OK;
    }

    if (ngx_http_ip_upload_not_be_limited(remote_addr,
            llcf->upload_min_intranet_ip, llcf->upload_max_intranet_ip) == NGX_OK) {
        return NGX_OK;
    }

    // 获得当前时间戳
    current_time = ngx_timeofday();
    curtime_msec = (ngx_msec_t)(current_time->sec * 1000 + current_time->msec);

    total_rate = 0;
    this_msec = 1;
    this_rate = 0;
    // 从配置中取出当前时段的限速，0不限
    ngx_http_get_current_interval_limit_rate(llcf, current_time->sec, &cur_limit_rate);
    if (cur_limit_rate == 0) {
        return NGX_OK;
    }

    // TODO:内核函数，后期改善，直接调用ngx_pid，打印日志有时不准
    ngx_pid_t this_pid = ngx_getpid();
    // 获得count，start_sec
    shm_zone = llcf->shm_zone;
    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
    shm_queue = (ngx_http_limit_upload_shm_queue_t *)shpool->data;

    ngx_shmtx_lock(&shpool->mutex);

    count = shm_queue->conn > 0 ? shm_queue->conn : 1;

    // 计算是否限制速度
    ngx_http_limit_upload_node_t *this_request_node = NULL;
    tmp_queue = shm_queue->queue;
    ngx_queue_t *p = tmp_queue->next;
    for (; p;) {
        tmp_node = ngx_queue_data(p, ngx_http_limit_upload_node_t, rq);
        if (tmp_node->r == r && tmp_node->processid == this_pid) {
            tmp_node->read += ngx_buf_size(buf);
            this_msec = curtime_msec - tmp_node->start_msec;
            this_msec = this_msec > 0 ? this_msec : 1;
            // byte/ms
            this_rate = tmp_node->read / this_msec;
            this_request_node = tmp_node;
        }
        msec = curtime_msec - tmp_node->start_msec;
        msec = msec > 0 ? msec : 1;
        total_rate += (tmp_node->read / msec);
        conns++;

        if (ngx_queue_last(tmp_queue) == p) {
            break;
        }
        p = ngx_queue_next(p);
    }
    conns = conns > 0 ? conns : 1;
    num = cur_limit_rate - total_rate * 1000;
    num = num / conns + this_rate * 1000;
    num = ((size_t)num > llcf->upload_min_limit_rate) 
        ? num : (ngx_int_t)(llcf->upload_min_limit_rate);
    num = ((size_t)num > cur_limit_rate) ? (ngx_int_t)cur_limit_rate : num;

    ngx_shmtx_unlock(&shpool->mutex);

    // 在共享内存中计算该request是否超速，并限速
    
   
    // TODO:这里改成当前时段的速度
    if (num > 0) {
        ctx->received += ngx_buf_size(buf);

        excess = ctx->received - llcf->limit_rate_after 
               - num * this_msec / 1000;

        ngx_log_debug8(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "upload limit conns[%d],"
            "total_rate[%l], this_rate[%l], num[%l], received[%l], excess[%d],"
            "cur_msec[%l], count[%d]",
            conns, total_rate,
            this_rate, num, ctx->received, excess,
            ngx_current_msec, count);

        if (excess > 0) {
            // 超出的字节数 / 限制的速度
            delay = excess * 1000 / num;

            if (delay < 20) {
                return NGX_OK;
            }

            // 对多次长时间delay的request豁免
            if (this_request_node != NULL) {
                // 在豁免区间
                if ((this_request_node->skip_limit & 0x01) == 0x01) {
                    if ((curtime_msec - this_request_node->delay_msec) >= 
                            (ngx_msec_t)(llcf->upload_limit_skip_msec)) {
                        this_request_node->skip_limit = 0x00;
                    } else {
                        return NGX_OK;
                    }
                } else {
                    // 不豁免
                    if (this_request_node->delay_count >= llcf->upload_limit_max_limit_count) {
                        // 开始豁免
                        this_request_node->skip_limit = 0x01;
                        this_request_node->delay_count = 0;
                        this_request_node->delay_msec = curtime_msec;
                        return NGX_OK;
                    }
                    if (delay > (ngx_msec_t)(llcf->upload_max_delay_time)) {
                        this_request_node->delay_count++;
                    }
                }
            }
 
            delay = delay > (ngx_msec_t)(llcf->upload_max_delay_time) ? 
                (ngx_msec_t)(llcf->upload_max_delay_time) : delay;

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "upload limit delay, delay_count[%d], max_limit_count[%d],"
                    "max_delay_time[%d], delay[%M]",
                    this_request_node->delay_count, llcf->upload_limit_max_limit_count,
                    llcf->upload_max_delay_time, delay);

            if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "limit upload: delay=%M", delay);

            ctx->read_event_handler = r->read_event_handler;
            ctx->write_event_handler = r->write_event_handler;
            r->write_event_handler = ngx_http_limit_upload_delay;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                    "r[%p], w[%p]", r->read_event_handler, r->write_event_handler);
            ngx_add_timer(r->connection->write, delay);
            if (!r->connection->read->ready) {
                clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
                ngx_add_timer(r->connection->read, clcf->client_body_timeout);

                if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

            } else if (r->connection->read->timer_set) {
                    ngx_del_timer(r->connection->read);
            }
            return NGX_AGAIN;
        }
    }

    return NGX_OK;
}

// 初始化共享内存
static ngx_int_t
ngx_http_limit_upload_init_zone(ngx_shm_zone_t *shm_zone, void *data) {
    size_t                               len = 0;
    ngx_slab_pool_t                     *shpool = NULL;
    ngx_http_limit_upload_shm_queue_t   *shm_queue = NULL;
    ngx_queue_t                         *tmp_queue = NULL;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    if (shm_zone->shm.exists) {
        return NGX_OK;
    }

    if (shpool != NULL && shpool->data != NULL) {
        return NGX_OK;
    }

    shm_queue = ngx_slab_alloc(shpool, sizeof(ngx_http_limit_upload_shm_queue_t));
    if (shm_queue == NULL) {
        return NGX_ERROR;
    }

    shpool->data = shm_queue;

    tmp_queue = ngx_slab_alloc(shpool, sizeof(ngx_queue_t));
    ngx_queue_init(tmp_queue);
    shm_queue->queue = tmp_queue;
    shm_queue->conn = 0;

    len = sizeof(" in limit_upload_module \"\"") + shm_zone->shm.name.len;
    shpool->log_ctx = ngx_slab_alloc(shpool, len);
    if (shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(shpool->log_ctx, " in limit_upload_module \"%V\"%Z",
            &shm_zone->shm.name);

    return NGX_OK;
}

static void *
ngx_http_limit_upload_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_limit_upload_conf_t  *conf;

    conf = ngx_palloc(cf->pool, sizeof(ngx_http_limit_upload_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->limit_rate_after = NGX_CONF_UNSET_SIZE;
    conf->upload_min_limit_rate = NGX_CONF_UNSET_SIZE;
    conf->remote_addr_index = NGX_CONF_UNSET;
    conf->upload_limit_rate_module_switch = NGX_CONF_UNSET;
    conf->shm_zone = NGX_CONF_UNSET_PTR;
    int i = 0;
    for (; i < 24; ++i) {
        conf->limit_rate_period[i] = NGX_CONF_UNSET_SIZE;
    }
    conf->upload_min_intranet_ip = NGX_CONF_UNSET_UINT;
    conf->upload_max_intranet_ip = NGX_CONF_UNSET_UINT;
    
    conf->upload_limit_max_limit_count = NGX_CONF_UNSET_UINT;
    conf->upload_limit_skip_msec = NGX_CONF_UNSET_UINT;
    conf->upload_max_delay_time = NGX_CONF_UNSET_UINT;

    return conf;
}

static char *
ngx_http_limit_upload_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_upload_conf_t  *conf = child;
    ngx_http_limit_upload_conf_t  *prev = parent;

    ngx_conf_merge_size_value(conf->limit_rate_after, prev->limit_rate_after,
                              0);
    ngx_conf_merge_size_value(conf->upload_min_limit_rate, prev->upload_min_limit_rate,
                              10240);
    ngx_conf_merge_value(conf->remote_addr_index, prev->remote_addr_index, -1);

    ngx_conf_merge_value(conf->upload_limit_rate_module_switch, 
                         prev->upload_limit_rate_module_switch, 0);

    ngx_conf_merge_ptr_value(conf->shm_zone, prev->shm_zone, NULL);

    int i = 0;
    for (; i < 24; ++i) {
        ngx_conf_merge_size_value(conf->limit_rate_period[i], 
            prev->limit_rate_period[i], 0);
    }
    
    ngx_conf_merge_uint_value(conf->upload_min_intranet_ip, 
        prev->upload_min_intranet_ip, 0);
    ngx_conf_merge_uint_value(conf->upload_max_intranet_ip,
        prev->upload_max_intranet_ip, 0);

    ngx_conf_merge_uint_value(conf->upload_limit_max_limit_count,
        prev->upload_limit_max_limit_count, 5);
    ngx_conf_merge_uint_value(conf->upload_limit_skip_msec,
        prev->upload_limit_skip_msec, 500);
    ngx_conf_merge_uint_value(conf->upload_max_delay_time,
        prev->upload_max_delay_time, 3000);

    return NGX_CONF_OK;
}

static void
ngx_http_limit_upload_cleanup(void *data) {
    ngx_http_limit_upload_cleanup_t      *lucln = data;
    ngx_shm_zone_t                       *shm_zone = lucln->shm_zone;
    ngx_http_request_t                   *r = lucln->r;
    ngx_pid_t                             processid = lucln->processid;

    ngx_slab_pool_t                      *shpool = NULL;
    ngx_http_limit_upload_shm_queue_t    *shm_queue = NULL;
    ngx_queue_t                          *tmp_queue = NULL;
    ngx_http_limit_upload_node_t         *tmp_node = NULL;

    // 在共享内存和queue中删除node
    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
    shm_queue = (ngx_http_limit_upload_shm_queue_t *)shpool->data;

    ngx_shmtx_lock(&shpool->mutex);

    tmp_queue = shm_queue->queue;
    ngx_queue_t *p = tmp_queue->next;
    for (; p;) {
        tmp_node = ngx_queue_data(p, ngx_http_limit_upload_node_t, rq);
        if (tmp_node->r == r && tmp_node->processid == processid) {
            tmp_node->r = NULL;
            ngx_queue_remove(p);
            shm_queue->conn--;
            ngx_slab_free_locked(shpool, tmp_node);
            break;
        }
        if (ngx_queue_last(tmp_queue) == p) {
            break;
        }
        p = ngx_queue_next(p);
    }

    ngx_shmtx_unlock(&shpool->mutex);
}

static ngx_int_t
ngx_http_limit_upload_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt           *h = NULL;
    ngx_http_core_main_conf_t     *cmcf = NULL;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_upload_handler;

    ngx_http_next_input_body_filter = ngx_http_top_input_body_filter;
    ngx_http_top_input_body_filter = ngx_http_limit_upload_input_body_filter;

    return NGX_OK;
}

static ngx_int_t
ngx_http_limit_upload_handler(ngx_http_request_t *r)
{
    ngx_http_limit_upload_ctx_t   *ctx = NULL;
    ngx_http_limit_upload_conf_t  *llcf = NULL;
    ngx_shm_zone_t                *shm_zone = NULL;
    ngx_slab_pool_t               *shpool = NULL;
    ngx_http_limit_upload_shm_queue_t   *shm_queue = NULL;
    ngx_queue_t                         *tmp_queue = NULL;
    ngx_http_limit_upload_node_t        *tmp_node = NULL;
    size_t                               remote_addr_len = 0;
    ngx_http_variable_value_t           *remote_addr = NULL;
    ngx_pool_cleanup_t                  *cln = NULL;
    ngx_http_limit_upload_cleanup_t     *lucln = NULL;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_limit_upload_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit upload: ctx=%p", ctx);

    ngx_http_set_ctx(r, ctx, ngx_http_limit_upload_module);

    // 判断ip是否需要限速
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_upload_module);
   
    if (!llcf->upload_limit_rate_module_switch) {
        return NGX_DECLINED;
    }

    if (r->method != NGX_HTTP_POST && r->method != NGX_HTTP_PUT) {
        return NGX_DECLINED;
    }

    remote_addr = ngx_http_get_indexed_variable(r, llcf->remote_addr_index);
    if (remote_addr == NULL || remote_addr->not_found) {
        return NGX_DECLINED;
    }

    remote_addr_len = remote_addr->len;

    if (remote_addr_len == 0) {
        return NGX_DECLINED;
    }

    if (remote_addr_len > 1024) {
        return NGX_DECLINED;
    }

    if (ngx_http_ip_upload_not_be_limited(remote_addr, 
            llcf->upload_min_intranet_ip, llcf->upload_max_intranet_ip) == NGX_OK) {
        return NGX_DECLINED;
    }

    ngx_pid_t this_pid = ngx_getpid();
    // 共享内存中插入本次请求
    shm_zone = llcf->shm_zone;
    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;
    shm_queue = (ngx_http_limit_upload_shm_queue_t *)shpool->data;

    ngx_shmtx_lock(&shpool->mutex);
    
    tmp_queue = shm_queue->queue;
    tmp_node = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_limit_upload_node_t));
    if (tmp_node == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    tmp_node->r = r;
    tmp_node->start_msec = (ngx_msec_t)(r->start_sec * 1000 + r->start_msec);
    tmp_node->processid = this_pid;
    tmp_node->read = 0;
    tmp_node->delay_count = 0;
    tmp_node->skip_limit = 0x00;
    tmp_node->delay_msec = (ngx_msec_t)(r->start_sec * 1000 + r->start_msec);
    ngx_queue_insert_tail(tmp_queue, &tmp_node->rq);
    shm_queue->conn++;
    
    ngx_shmtx_unlock(&shpool->mutex);

    // 通过cleanup来释放共享内存的元素
    cln = ngx_pool_cleanup_add(r->pool,
            sizeof(ngx_http_limit_upload_cleanup_t));
    
    if (NULL == cln) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    cln->handler = ngx_http_limit_upload_cleanup;
    lucln = cln->data;

    lucln->shm_zone = shm_zone;
    lucln->r = r;
    lucln->processid = this_pid;

    return NGX_DECLINED;
}

static void
ngx_http_limit_upload_delay(ngx_http_request_t *r)
{
    ngx_event_t                   *wev = NULL;
    ngx_http_limit_upload_ctx_t   *ctx = NULL;

    ngx_time_t *current_time = ngx_timeofday();

    ctx = ngx_http_get_module_ctx(r, ngx_http_limit_upload_module);

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit upload delay, sec[%l] msec[%l] "
                   "ctx->r[%p], ctx->w[%p]",
                   current_time->sec, current_time->msec,
                   ctx->read_event_handler,
                   ctx->write_event_handler);

    wev = r->connection->write;

    if (!wev->timedout) {

        if (ngx_handle_write_event(wev, 0) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        }

        return;
    }

    wev->timedout = 0;

    if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    r->read_event_handler(r);
}

static char *
ngx_http_limit_upload_rate_period(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf) {
    ngx_http_limit_upload_conf_t *lucf = conf;

    ngx_str_t  item;
    ngx_str_t *rate_str = NULL;
    ngx_str_t *value = NULL;
    ngx_str_t  binary_remote_str;
    size_t     str_len = 0; 
    size_t     start = 0;
    size_t     index = 0; 
    size_t     item_count = 0;
    size_t     n = 0;
    u_char    *str_data = NULL;

    value = cf->args->elts;
    rate_str = &value[1];
    str_len = rate_str->len;
    str_data = rate_str->data;

    start = item_count = index = 0;
    if (str_len == 0 || str_data[0] == '_' || str_data[str_len - 1] == '_') {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
            "limit_upload_rate_period error");
        return NGX_CONF_ERROR;
    }
    while (index < str_len && item_count < 23) {
        if (str_data[index] == '_') {
            item.data = &str_data[start];
            item.len = index - start;
            n = ngx_parse_size(&item);
            lucf->limit_rate_period[item_count++] = n;

            start = index + 1;
        }
        index++;
    }
    // 剩余最后一组
    if (item_count != 23) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
            "limit_upload_rate_period num[%lu] error", item_count);
        return NGX_CONF_ERROR;
    }
    item.data = &str_data[start];
    item.len = str_len - start;
    n = ngx_parse_size(&item);
    lucf->limit_rate_period[item_count] = n;

    // 记录ip地址
    ngx_str_set(&binary_remote_str, "binary_remote_addr");
    lucf->remote_addr_index = ngx_http_get_variable_index(cf, &binary_remote_str);
    if (lucf->remote_addr_index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

// 加载内网ip的最小和最大值
static char *
ngx_http_limit_upload_intranet_ip(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf) {
    ngx_http_limit_upload_conf_t *lucf = conf;

    ngx_str_t  *min_ip_str = NULL;
    ngx_str_t  *max_ip_str = NULL;
    ngx_str_t  *value = NULL;

    value = cf->args->elts;
    min_ip_str = &value[1];
    max_ip_str = &value[2];

    lucf->upload_min_intranet_ip = ntohl(ngx_inet_addr(min_ip_str->data, min_ip_str->len));
    lucf->upload_max_intranet_ip = ntohl(ngx_inet_addr(max_ip_str->data, max_ip_str->len));

    return NGX_CONF_OK;
}

static char *
ngx_http_limit_upload_rate_shm_zone(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf) {
    // main级别
    ngx_http_limit_upload_conf_t *lucf = NULL;
    lucf = conf;
    if (lucf == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "upload zone ngx_http_limit_upload_conf_t is NULL");
        return NGX_CONF_ERROR;
    }

    ngx_str_t *value = NULL;
    ngx_str_t *shm_name = NULL;
    ngx_str_t *shm_size_str = NULL;
    value = cf->args->elts;
    shm_name = &value[1];
    shm_size_str = &value[2];

    ssize_t n = 0;
    n = ngx_parse_size(shm_size_str);
    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid size of limit_upload_zone \"%V\"", shm_size_str);
        return NGX_CONF_ERROR;
    }

    ngx_shm_zone_t *shm_zone = NULL;
    shm_zone = ngx_shared_memory_add(cf, shm_name, n,
            &ngx_http_limit_upload_module);
    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "upload zone ngx_shared_memory_add failed");
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_limit_upload_init_zone;
    shm_zone->data = NULL;
    lucf->shm_zone = shm_zone;

    return NGX_CONF_OK;
}
