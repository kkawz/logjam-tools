#include <zmq.h>
#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <json-c/json.h>
#include <bson.h>
#include <mongoc.h>

#ifdef DEBUG
#undef DEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

// TODO:
// way more json input validation
// more assertions, return code checking

static inline
void log_zmq_error(int rc)
{
  if (rc != 0) {
      fprintf(stderr, "[E] rc: %d, errno: %d (%s)\n", rc, errno, zmq_strerror(errno));
  }
}

/* global config */
static zconfig_t* config = NULL;
static zfile_t *config_file = NULL;
static char *config_file_name = "logjam.conf";
static time_t config_file_last_modified = 0;
static char *config_file_digest = "";
static bool dryrun = false;
static char *subscription_pattern = "";

void config_file_init()
{
    config_file = zfile_new(NULL, config_file_name);
    config_file_last_modified = zfile_modified(config_file);
    config_file_digest = strdup(zfile_digest(config_file));
}

bool config_file_has_changed()
{
    bool changed = false;
    zfile_restat(config_file);
    if (config_file_last_modified != zfile_modified(config_file)) {
        // bug in czmq: does not reset digest on restat
        zfile_t *tmp = zfile_new(NULL, config_file_name);
        char *new_digest = zfile_digest(tmp);
        // printf("[D] old digest: %s\n[D] new digest: %s\n", config_file_digest, new_digest);
        changed = strcmp(config_file_digest, new_digest) != 0;
        zfile_destroy(&tmp);
    }
    return changed;
}


#define ISO_DATE_STR_LEN 11
static char iso_date_today[ISO_DATE_STR_LEN] = {'0'};
static char iso_date_tomorrow[ISO_DATE_STR_LEN] = {'0'};
static time_t time_last_tick = 0;
// discard all messages which differ by more than 1 hour from the current time
// if we have larger clockdrift: tough luck
#define INVALID_MSG_AGE_THRESHOLD 3600

bool update_date_info()
{
    char old_date[ISO_DATE_STR_LEN];
    strcpy(old_date, iso_date_today);

    time_last_tick = time(NULL);
    struct tm lt;
    assert( localtime_r(&time_last_tick, &lt) );
    // calling mktime fills in potentially missing TZ and DST info
    assert( mktime(&lt) != -1 );
    sprintf(iso_date_today,  "%04d-%02d-%02d", 1900 + lt.tm_year, 1 + lt.tm_mon, lt.tm_mday);

    // calculate toomorrows date
    lt.tm_mday += 1;
    assert( mktime(&lt) != -1 );

    sprintf(iso_date_tomorrow,  "%04d-%02d-%02d", 1900 + lt.tm_year, 1 + lt.tm_mon, lt.tm_mday);

    // printf("[D] today's    ISO date is %s\n", iso_date_today);
    // printf("[D] tomorrow's ISO date is %s\n", iso_date_tomorrow);
    bool changed = strcmp(old_date, iso_date_today);
    return changed;
}

#define MAX_DATABASES 100
#define DEFAULT_MONGO_URI "mongodb://127.0.0.1:27017/"
static size_t num_databases = 0;
static const char *databases[MAX_DATABASES];

typedef struct {
    const char* name;
    size_t value;
} module_threshold_t;

typedef struct {
    const char *key;
    const char *app;
    const char *env;
    size_t key_len;
    size_t app_len;
    size_t env_len;
    int db;
    int import_threshold;
    int module_threshold_count;
    module_threshold_t *module_thresholds;
    const char *ignored_request_prefix;
} stream_info_t;

static int global_total_time_import_threshold = 0;
static const char* global_ignored_request_prefix = NULL;

// utf8 conversion
static char UTF8_DOT[4] = {0xE2, 0x80, 0xA4, '\0' };
static char UTF8_CURRENCY[3] = {0xC2, 0xA4, '\0'};
static char *URI_ESCAPED_DOT = "%2E";
static char *URI_ESCAPED_DOLLAR = "%24";

/* resource maps */
#define MAX_RESOURCE_COUNT 100
static zhash_t* resource_to_int = NULL;
static char *int_to_resource[MAX_RESOURCE_COUNT];
static char *int_to_resource_sq[MAX_RESOURCE_COUNT];
static size_t last_resource_index = 0;

static char *time_resources[MAX_RESOURCE_COUNT];
static size_t last_time_resource_index = 0;

static char *other_time_resources[MAX_RESOURCE_COUNT];
static size_t last_other_time_resource_index = 0;

static char *call_resources[MAX_RESOURCE_COUNT];
static size_t last_call_resource_index = 0;

static char *memory_resources[MAX_RESOURCE_COUNT];
static size_t last_memory_resource_index = 0;

static char *heap_resources[MAX_RESOURCE_COUNT];
static size_t last_heap_resource_index = 0;

static char *frontend_resources[MAX_RESOURCE_COUNT];
static size_t last_frontend_resource_index = 0;

static char *dom_resources[MAX_RESOURCE_COUNT];
static size_t last_dom_resource_index = 0;

static size_t allocated_objects_index, allocated_bytes_index;

static inline size_t r2i(const char* resource)
{
    return (size_t)zhash_lookup(resource_to_int, resource);
}

static inline const char* i2r(size_t i)
{
    assert(i <= last_resource_index);
    return (const char*)(int_to_resource[i]);
}

/* msg stats */
typedef struct {
    size_t transmitted;
    size_t dropped;
} msg_stats_t;

#if DEBUG == 1
#define ONLY_ONE_THREAD_EACH
#endif

#ifdef ONLY_ONE_THREAD_EACH
#define NUM_PARSERS 1
#define NUM_UPDATERS 1
#define NUM_WRITERS 1
#else
#define NUM_PARSERS 4
#define NUM_UPDATERS 10
#define NUM_WRITERS 10
#endif

/* controller state */
typedef struct {
    void *subscriber_pipe;
    void *indexer_pipe;
    void *parser_pipes[NUM_PARSERS];
    void *writer_pipes[NUM_WRITERS];
    void *updater_pipes[NUM_UPDATERS];
    void *updates_socket;
    void *live_stream_socket;
    size_t ticks;
} controller_state_t;

/* subscriber state */
typedef struct {
    void *controller_socket;
    void *sub_socket;
    void *push_socket;
    void *pull_socket;
    void *pub_socket;
} subscriber_state_t;

/* parser state */
typedef struct {
    size_t id;
    size_t parsed_msgs_count;
    void *controller_socket;
    void *pull_socket;
    void *push_socket;
    void *indexer_socket;
    json_tokener* tokener;
    zhash_t *processors;
} parser_state_t;

/* processor state */
typedef struct {
    char *db_name;
    stream_info_t* stream_info;
    size_t request_count;
    zhash_t *modules;
    zhash_t *totals;
    zhash_t *minutes;
    zhash_t *quants;
} processor_state_t;

/* request info */
typedef struct {
    const char* page;
    const char* module;
    double total_time;
    int response_code;
    int severity;
    int minute;
    int heap_growth;
    json_object* exceptions;
} request_data_t;

/* increments */
// TODO: support integer vlaues (for call metrics)
typedef struct {
    double val;
    double val_squared;
} metric_pair_t;

typedef struct {
    size_t backend_request_count;
    size_t page_request_count;
    size_t ajax_request_count;
    metric_pair_t *metrics;
    json_object *others;
} increments_t;

#define METRICS_ARRAY_SIZE (sizeof(metric_pair_t) * (last_resource_index + 1))
#define QUANTS_ARRAY_SIZE (sizeof(size_t) * (last_resource_index + 1))

typedef struct {
    mongoc_collection_t *totals;
    mongoc_collection_t *minutes;
    mongoc_collection_t *quants;
} stats_collections_t;

/* indexer state */
typedef struct {
    size_t id;
    mongoc_client_t *mongo_clients[MAX_DATABASES];
    mongoc_collection_t *global_collection;
    void *controller_socket;
    void *pull_socket;
    zhash_t *databases;
} indexer_state_t;

/* stats updater state */
typedef struct {
    size_t id;
    mongoc_client_t *mongo_clients[MAX_DATABASES];
    mongoc_collection_t *global_collection;
    zhash_t *stats_collections;
    void *controller_socket;
    void *pull_socket;
    void *push_socket;
    size_t updates_count;
} stats_updater_state_t;

/* request writer state */
typedef struct {
    size_t id;
    mongoc_client_t* mongo_clients[MAX_DATABASES];
    zhash_t *request_collections;
    zhash_t *jse_collections;
    zhash_t *events_collections;
    void *controller_socket;
    void *pull_socket;
    void *push_socket;
    void *live_stream_socket;
    size_t request_count;
} request_writer_state_t;

typedef struct {
    zhash_t *source;
    zhash_t *target;
} hash_pair_t;

/* collection updater callback struct */
typedef struct {
    const char *db_name;
    mongoc_collection_t *collection;
} collection_update_callback_t;

static mongoc_write_concern_t *wc_no_wait = NULL;
static mongoc_write_concern_t *wc_wait = NULL;
static mongoc_index_opt_t index_opt_default;
static mongoc_index_opt_t index_opt_sparse;

#define USE_UNACKNOWLEDGED_WRITES 0
#define USE_BACKGROUND_INDEX_BUILDS 1
#define TOKU_TX_LOCK_FAILED 16759
#define TOKU_TX_RETRIES 2

#if USE_UNACKNOWLEDGED_WRITES == 1
#define USE_PINGS true
#else
#define USE_PINGS false
#endif

// these are all tick counts
#define PING_INTERVAL 5
#define COLLECTION_REFRESH_INTERVAL 3600
#define CONFIG_FILE_CHECK_INTERVAL 10

void my_mongo_log_handler(mongoc_log_level_t log_level, const char *log_domain, const char *message, void *user_data)
{
   FILE *stream;
   char level = 'I';

   switch (log_level) {
   case MONGOC_LOG_LEVEL_ERROR:
   case MONGOC_LOG_LEVEL_CRITICAL:
       level = 'E';
       stream = stderr;
       break;
   case MONGOC_LOG_LEVEL_WARNING:
       level = 'W';
       stream = stderr;
       break;
   case MONGOC_LOG_LEVEL_MESSAGE:
   case MONGOC_LOG_LEVEL_INFO:
       level = 'I';
       stream = stdout;
       break;
   case MONGOC_LOG_LEVEL_DEBUG:
   case MONGOC_LOG_LEVEL_TRACE:
       level = 'D';
       stream = stdout;
       break;
   default:
       stream = stdout;
   }

   fprintf(stream, "[%c] monogc[%s]: %s\n", level, log_domain, message);
}

void initialize_mongo_db_globals()
{
    mongoc_init();
    mongoc_log_set_handler(my_mongo_log_handler, NULL);

    wc_wait = mongoc_write_concern_new();
    mongoc_write_concern_set_w(wc_wait, MONGOC_WRITE_CONCERN_W_DEFAULT);

    wc_no_wait = mongoc_write_concern_new();
    if (USE_UNACKNOWLEDGED_WRITES)
        // TODO: this leads to illegal opcodes on the server
       mongoc_write_concern_set_w(wc_no_wait, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
    else
        mongoc_write_concern_set_w(wc_no_wait, MONGOC_WRITE_CONCERN_W_DEFAULT);

    mongoc_index_opt_init(&index_opt_default);
    if (USE_BACKGROUND_INDEX_BUILDS)
        index_opt_default.background = true;
    else
        index_opt_default.background = false;

    mongoc_index_opt_init(&index_opt_sparse);
    index_opt_sparse.sparse = true;
    if (USE_BACKGROUND_INDEX_BUILDS)
        index_opt_sparse.background = true;
    else
        index_opt_sparse.background = false;

    zconfig_t* dbs = zconfig_locate(config, "backend/databases");
    if (dbs) {
        zconfig_t *db = zconfig_child(dbs);
        while (db) {
            assert(num_databases < MAX_DATABASES);
            char *uri = zconfig_value(db);
            if (uri != NULL) {
                databases[num_databases] = strdup(uri);
                printf("[I] database[%zu]: %s\n", num_databases, uri);
                num_databases++;
            }
            db = zconfig_next(db);
        }
    }
    if (num_databases == 0) {
        databases[num_databases] = DEFAULT_MONGO_URI;
        printf("[I] database[%zu]: %s\n", num_databases, DEFAULT_MONGO_URI);
        num_databases++;
    }
}

bool output_socket_ready(void *socket, int msecs)
{
    zmq_pollitem_t items[] = { { socket, 0, ZMQ_POLLOUT, 0 } };
    int rc = zmq_poll(items, 1, msecs);
    return rc != -1 && (items[0].revents & ZMQ_POLLOUT) != 0;
}

void connect_multiple(void* socket, const char* name, int which)
{
    for (int i=0; i<which; i++) {
        // TODO: HACK!
        int rc;
        for (int j=0; j<10; j++) {
            rc = zsocket_connect(socket, "inproc://%s-%d", name, i);
            if (rc == 0) break;
            zclock_sleep(100); // ms
        }
        log_zmq_error(rc);
        assert(rc == 0);
    }
}

void* live_stream_socket_new(zctx_t *context)
{
    void *live_stream_socket = zsocket_new(context, ZMQ_PUSH);
    assert(live_stream_socket);
    int rc = zsocket_connect(live_stream_socket, "tcp://localhost:9607");
    assert(rc == 0);
    return live_stream_socket;
}

void live_stream_publish(void *live_stream_socket, const char* key, const char* json_str)
{
    int rc = 0;
    zframe_t *msg_key = zframe_new(key, strlen(key));
    zframe_t *msg_body = zframe_new(json_str, strlen(json_str));
    rc = zframe_send(&msg_key, live_stream_socket, ZFRAME_MORE|ZFRAME_DONTWAIT);
    // printf("[D] MSG frame 1 to live stream: rc=%d\n", rc);
    if (rc == 0) {
        rc = zframe_send(&msg_body, live_stream_socket, ZFRAME_DONTWAIT);
        // printf("[D] MSG frame 2 to live stream: rc=%d\n", rc);
    } else {
        zframe_destroy(&msg_body);
    }
}

// all configured streams
static zhash_t *configured_streams = NULL;
// all streams we want to subscribe to
static zhash_t *stream_subscriptions = NULL;

void* subscriber_sub_socket_new(zctx_t *context)
{
    void *socket = zsocket_new(context, ZMQ_SUB);
    assert(socket);
    zsocket_set_rcvhwm(socket, 10000);
    zsocket_set_linger(socket, 0);
    zsocket_set_reconnect_ivl(socket, 100); // 100 ms
    zsocket_set_reconnect_ivl_max(socket, 10 * 1000); // 10 s

    // connect socket to endpoints
    zconfig_t *endpoints = zconfig_locate(config, "frontend/endpoints");
    assert(endpoints);
    zconfig_t *endpoint = zconfig_child(endpoints);
    assert(endpoint);
    do {
        zconfig_t *binding = zconfig_child(endpoint);
        assert(binding);
        do {
            char *spec = zconfig_value(binding);
            int rc = zsocket_connect(socket, "%s", spec);
            log_zmq_error(rc);
            assert(rc == 0);
            binding = zconfig_next(binding);
        } while (binding);
        endpoint = zconfig_next(endpoint);
    } while (endpoint);

    return socket;
}

static char *direct_bind_ip = "*";
static int direct_bind_port = 9605;

void* subscriber_pull_socket_new(zctx_t *context)
{
    void *socket = zsocket_new(context, ZMQ_PULL);
    assert(socket);
    zsocket_set_rcvhwm(socket, 10000);
    zsocket_set_linger(socket, 0);
    zsocket_set_reconnect_ivl(socket, 100); // 100 ms
    zsocket_set_reconnect_ivl_max(socket, 10 * 1000); // 10 s

    // connect socket to endpoints
    // TODO: read bind_ip and port from config
    int rc = zsocket_bind(socket, "tcp://%s:%d", direct_bind_ip, direct_bind_port);
    assert(rc == direct_bind_port);

    return socket;
}

void* subscriber_push_socket_new(zctx_t *context)
{
    void *socket = zsocket_new(context, ZMQ_PUSH);
    assert(socket);
    int rc = zsocket_bind(socket, "inproc://subscriber");
    assert(rc == 0);
    return socket;
}

void* subscriber_pub_socket_new(zctx_t *context)
{
    void *socket = zsocket_new(context, ZMQ_PUB);  /* testing */
    assert(socket);
    zsocket_set_sndhwm(socket, 200000);
    int rc = zsocket_bind(socket, "tcp://*:%d", 9651);
    assert(rc == 9651);
    return socket;
}

void subscriber_publish_duplicate(zmsg_t *msg, void *socket)
{
    static size_t seq = 0;
    // zmsg_send(&msg_copy, state->pub_socket);
    zmsg_t *msg_copy = zmsg_dup(msg);
    zmsg_addstrf(msg_copy, "%zu", ++seq);
    zframe_t *frame = zmsg_pop(msg_copy);
    while (frame != NULL) {
        zframe_t *next_frame = zmsg_pop(msg_copy);
        int more = next_frame ? ZFRAME_MORE : 0;
        // zframe_print(frame, "DUP");
        if (zframe_send(&frame, socket, ZFRAME_DONTWAIT|more) == -1)
            break;
        frame = next_frame;
    }
    zmsg_destroy(&msg_copy);
}

int read_request_and_forward(zloop_t *loop, zmq_pollitem_t *item, void *callback_data)
{
    subscriber_state_t *state = callback_data;
    zmsg_t *msg = zmsg_recv(item->socket);
    if (msg != NULL) {
        // zmsg_dump(msg);
        // subscriber_publish_duplicate(msg, state->pub_socket);
        zmsg_send(&msg, state->push_socket);
    }
    return 0;
}

void subscriber(void *args, zctx_t *ctx, void *pipe)
{
    int rc;
    subscriber_state_t state;
    state.controller_socket = pipe;
    state.sub_socket = subscriber_sub_socket_new(ctx);
    state.pull_socket = subscriber_pull_socket_new(ctx);
    state.push_socket = subscriber_push_socket_new(ctx);
    state.pub_socket = subscriber_pub_socket_new(ctx);

    if (zhash_size(stream_subscriptions) == 0) {
        // subscribe to all messages
        zsocket_set_subscribe(state.sub_socket, "");
    } else {
        // setup subscriptions for only a subset
        zlist_t *subscriptions = zhash_keys(stream_subscriptions);
        char *stream = zlist_first(subscriptions);
        while (stream != NULL)  {
            printf("[I] controller: subscribing to stream: %s\n", stream);
            zsocket_set_subscribe(state.sub_socket, stream);
            size_t n = strlen(stream);
            if (n > 15 && !strncmp(stream, "request-stream-", 15)) {
                zsocket_set_subscribe(state.sub_socket, stream+15);
            } else {
                char old_stream[n+15+1];
                sprintf(old_stream, "request-stream-%s", stream);
                zsocket_set_subscribe(state.sub_socket, old_stream);
            }
            stream = zlist_next(subscriptions);
        }
        zlist_destroy(&subscriptions);
    }

    // set up event loop
    zloop_t *loop = zloop_new();
    assert(loop);
    zloop_set_verbose(loop, 0);

     // setup handler for the sub socket
    zmq_pollitem_t sub_item;
    sub_item.socket = state.sub_socket;
    sub_item.events = ZMQ_POLLIN;
    rc = zloop_poller(loop, &sub_item, read_request_and_forward, &state);
    assert(rc == 0);

    // setup handler for the pull socket
    zmq_pollitem_t pull_item;
    pull_item.socket = state.pull_socket;
    pull_item.events = ZMQ_POLLIN;
    rc = zloop_poller(loop, &pull_item, read_request_and_forward, &state);
    assert(rc == 0);

    // run the loop
    rc = zloop_start(loop);
    // printf("[D] zloop return: %d", rc);

    // shutdown
    zloop_destroy(&loop);
    assert(loop == NULL);
}

#define DB_PREFIX "logjam-"
#define DB_PREFIX_LEN 7

processor_state_t* processor_new(char *db_name)
{
    // printf("[D] creating processor for db: %s\n", db_name);
    // check whether it's a known stream and return NULL if not
    size_t n = strlen(db_name) - DB_PREFIX_LEN;
    char stream_name[n+1];
    strcpy(stream_name, db_name + DB_PREFIX_LEN);
    stream_name[n-11] = '\0';

    stream_info_t *stream_info = zhash_lookup(configured_streams, stream_name);
    if (stream_info == NULL) {
        fprintf(stderr, "[E] did not find stream info: %s\n", stream_name);
        return NULL;
    }
    // printf("[D] found stream info for stream %s: %s\n", stream_name, stream_info->key);

    processor_state_t *p = malloc(sizeof(processor_state_t));
    p->db_name = strdup(db_name);
    p->stream_info = stream_info;
    p->request_count = 0;
    p->modules = zhash_new();
    p->totals = zhash_new();
    p->minutes = zhash_new();
    p->quants = zhash_new();
    return p;
}

void processor_destroy(void* processor)
{
    //void* because we want to use it as a zhash_free_fn
    processor_state_t* p = processor;
    // printf("[D] destroying processor: %s. requests: %zu\n", p->stream, p->request_count);
    free(p->db_name);
    if (p->modules != NULL)
        zhash_destroy(&p->modules);
    if (p->totals != NULL)
        zhash_destroy(&p->totals);
    if (p->minutes != NULL)
        zhash_destroy(&p->minutes);
    if (p->quants != NULL)
        zhash_destroy(&p->quants);
    free(p);
}

#define INVALID_DATE -1
time_t valid_database_date(const char *date)
{
    if (strlen(date) < 19) {
        fprintf(stderr, "[E] detected crippled date string: %s\n", date);
        return INVALID_DATE;
    }
    struct tm time;
    memset(&time, 0, sizeof(time));
    // fill in correct TZ and DST info
    localtime_r(&time_last_tick, &time);
    const char* format = date[10] == 'T' ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%d %H:%M:%S";
    if (!strptime(date, format, &time)) {
        fprintf(stderr, "[E] could not parse date: %s\n", date);
        return INVALID_DATE;
    }
    time_t res = mktime(&time);

    // char b[100];
    // ctime_r(&time_last_tick, b);
    // puts(b);
    // ctime_r(&res, b);
    // puts(b);

    int drift = abs( difftime (res,time_last_tick) );
    if ( drift > INVALID_MSG_AGE_THRESHOLD) {
        fprintf(stderr, "[E] detected intolerable clock drift: %d seconds\n", drift);
        return INVALID_DATE;
    }
    else
        return res;
}

processor_state_t* processor_create(zframe_t* stream_frame, parser_state_t* parser_state, json_object *request)
{
    size_t n = zframe_size(stream_frame);
    char db_name[n+100];
    strcpy(db_name, "logjam-");
    // printf("[D] db_name: %s\n", db_name);

    const char *stream_chars = (char*)zframe_data(stream_frame);
    if (n > 15 && !strncmp("request-stream-", stream_chars, 15)) {
        memcpy(db_name+7, stream_chars+15, n-15);
        db_name[n+7-15] = '-';
        db_name[n+7-14] = '\0';
    } else {
        memcpy(db_name+7, stream_chars, n);
        db_name[n+7] = '-';
        db_name[n+7+1] = '\0';
    }
    // printf("[D] db_name: %s\n", db_name);

    json_object* started_at_value;
    if (!json_object_object_get_ex(request, "started_at", &started_at_value)) {
        fprintf(stderr, "[E] dropped request without started_at date\n");
        return NULL;
    }
    const char *date_str = json_object_get_string(started_at_value);
    if (INVALID_DATE == valid_database_date(date_str)) {
        fprintf(stderr, "[E] dropped request for %s with invalid started_at date: %s\n", db_name, date_str);
        return NULL;
    }
    strncpy(&db_name[n+7+1], date_str, 10);
    db_name[n+7+1+10] = '\0';
    // printf("[D] db_name: %s\n", db_name);

    processor_state_t *p = zhash_lookup(parser_state->processors, db_name);
    if (p == NULL) {
        p = processor_new(db_name);
        if (p) {
            int rc = zhash_insert(parser_state->processors, db_name, p);
            assert(rc ==0);
            zhash_freefn(parser_state->processors, db_name, processor_destroy);
            // send msg to indexer to create db indexes
            zmsg_t *msg = zmsg_new();
            assert(msg);
            zmsg_addstr(msg, db_name);
            zmsg_addmem(msg, &p->stream_info, sizeof(stream_info_t*));
            zmsg_send(&msg, parser_state->indexer_socket);
        }
    }
    return p;
}

void* parser_pull_socket_new(zctx_t *context)
{
    int rc;
    void *socket = zsocket_new(context, ZMQ_PULL);
    assert(socket);
    // connect socket, taking thread startup time into account
    // TODO: this is a hack. better let controller coordinate this
    for (int i=0; i<10; i++) {
        rc = zsocket_connect(socket, "inproc://subscriber");
        if (rc == 0) break;
        zclock_sleep(100);
    }
    log_zmq_error(rc);
    assert(rc == 0);
    return socket;
}

void* parser_push_socket_new(zctx_t *context)
{
    void *socket = zsocket_new(context, ZMQ_PUSH);
    assert(socket);
    connect_multiple(socket, "request-writer", NUM_WRITERS);
    return socket;
}

void* parser_indexer_socket_new(zctx_t *context)
{
    void *socket = zsocket_new(context, ZMQ_PUSH);
    assert(socket);
    int rc = zsocket_connect(socket, "inproc://indexer");
    assert (rc == 0);
    return socket;
}

void dump_json_object(FILE *f, json_object *jobj) {
    const char *json_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    if (f == stderr)
        fprintf(f, "[E] %s\n", json_str);
    else
        fprintf(f, "[I] %s\n", json_str);
    // don't try to free the json string. it will crash.
}

void my_zframe_fprint(zframe_t *self, const char *prefix, FILE *file)
{
    assert (self);
    if (prefix)
        fprintf (file, "%s", prefix);
    byte *data = zframe_data (self);
    size_t size = zframe_size (self);

    int is_bin = 0;
    uint char_nbr;
    for (char_nbr = 0; char_nbr < size; char_nbr++)
        if (data [char_nbr] < 9 || data [char_nbr] > 127)
            is_bin = 1;

    fprintf (file, "[%03d] ", (int) size);
    size_t max_size = is_bin? 2048: 4096;
    const char *ellipsis = "";
    if (size > max_size) {
        size = max_size;
        ellipsis = "...";
    }
    for (char_nbr = 0; char_nbr < size; char_nbr++) {
        if (is_bin)
            fprintf (file, "%02X", (unsigned char) data [char_nbr]);
        else
            fprintf (file, "%c", data [char_nbr]);
    }
    fprintf (file, "%s\n", ellipsis);
}

void my_zmsg_fprint(zmsg_t* self, const char* prefix, FILE* file)
{
    zframe_t *frame = zmsg_first(self);
    int frame_nbr = 0;
    while (frame && frame_nbr++ < 10) {
        my_zframe_fprint(frame, prefix, file);
        frame = zmsg_next(self);
    }
}

json_object* parse_json_body(zframe_t *body, json_tokener* tokener)
{
    char* json_data = (char*)zframe_data(body);
    int json_data_len = (int)zframe_size(body);
    json_tokener_reset(tokener);
    json_object *jobj = json_tokener_parse_ex(tokener, json_data, json_data_len);
    enum json_tokener_error jerr = json_tokener_get_error(tokener);
    if (jerr != json_tokener_success) {
        fprintf(stderr, "[E] parse_json_body: %s\n", json_tokener_error_desc(jerr));
    } else {
        // const char *json_str_orig = zframe_strdup(body);
        // printf("[D] %s\n", json_str_orig);
        // free(json_str_orig);
        // dump_json_object(stdout, jobj);
    }
    if (tokener->char_offset < json_data_len) // XXX shouldn't access internal fields
    {
        // Handle extra characters after parsed object as desired.
        fprintf(stderr, "[W] parse_json_body: %s\n", "extranoeus data in message payload");
        my_zframe_fprint(body, "[W] MSGBODY=", stderr);
    }
    // if (strnlen(json_data, json_data_len) < json_data_len) {
    //     fprintf(stderr, "[W] parse_json_body: json payload has null bytes\ndata: %*s\n", json_data_len, json_data);
    //     dump_json_object(stdout, jobj);
    //     return NULL;
    // }
    return jobj;
}

increments_t* increments_new()
{
    const size_t inc_size = sizeof(increments_t);
    increments_t* increments = malloc(inc_size);
    memset(increments, 0, inc_size);

    const size_t metrics_size = METRICS_ARRAY_SIZE;
    increments->metrics = malloc(metrics_size);
    memset(increments->metrics, 0, metrics_size);

    increments->others = json_object_new_object();
    return increments;
}

void increments_destroy(void *increments)
{
    // void* because of zhash_destroy
    increments_t *incs = increments;
    json_object_put(incs->others);
    free(incs->metrics);
    free(incs);
}

increments_t* increments_clone(increments_t* increments)
{
    increments_t* new_increments = increments_new();
    new_increments->backend_request_count = increments->backend_request_count;
    new_increments->page_request_count = increments->page_request_count;
    new_increments->ajax_request_count = increments->ajax_request_count;
    memcpy(new_increments->metrics, increments->metrics, METRICS_ARRAY_SIZE);
    json_object_object_foreach(increments->others, key, value) {
        json_object_get(value);
        json_object_object_add(new_increments->others, key, value);
    }
    return new_increments;
}

void increments_fill_metrics(increments_t *increments, json_object *request)
{
    const int n = last_resource_index;
    for (size_t i=0; i <= n; i++) {
        json_object* metrics_value;
        if (json_object_object_get_ex(request, int_to_resource[i], &metrics_value)) {
            double v = json_object_get_double(metrics_value);
            metric_pair_t *p = &increments->metrics[i];
            p->val = v;
            p->val_squared = v*v;
        }
    }
}

void increments_add_metrics_to_json(increments_t *increments, json_object *jobj)
{
    const int n = last_resource_index;
    for (size_t i=0; i <= n; i++) {
        metric_pair_t *p = &increments->metrics[i];
        double v = p->val;
        if (v > 0) {
            json_object_object_add(jobj, int_to_resource[i], json_object_new_double(v));
        }
    }
}

#define NEW_INT1 (json_object_new_int(1))


void increments_fill_apdex(increments_t *increments, request_data_t *request_data)
{
    double total_time = request_data->total_time;
    long response_code = request_data->response_code;
    json_object *others = increments->others;

    if (total_time >= 2000 || response_code >= 500) {
        // TODO: users are certainly unhappy when response code is 500. but should really use that here?
        json_object_object_add(others, "apdex.frustrated", NEW_INT1);
    } else if (total_time < 100) {
        json_object_object_add(others, "apdex.happy", NEW_INT1);
        json_object_object_add(others, "apdex.satisfied", NEW_INT1);
    } else if (total_time < 500) {
        json_object_object_add(others, "apdex.satisfied", NEW_INT1);
    } else if (total_time < 2000) {
        json_object_object_add(others, "apdex.tolerating", NEW_INT1);
    }
}

void increments_fill_frontend_apdex(increments_t *increments, double total_time)
{
    json_object *others = increments->others;

    if (total_time < 2000) {
        json_object_object_add(others, "fapdex.satisfied", NEW_INT1);
    } else if (total_time < 8000) {
        json_object_object_add(others, "fapdex.tolerating", NEW_INT1);
    } else {
        json_object_object_add(others, "fapdex.frustrated", NEW_INT1);
    }
}

void increments_fill_response_code(increments_t *increments, request_data_t *request_data)
{
    char rsp[256];
    snprintf(rsp, 256, "response.%d", request_data->response_code);
    json_object_object_add(increments->others, rsp, NEW_INT1);
}

void increments_fill_severity(increments_t *increments, request_data_t *request_data)
{
    char sev[256];
    snprintf(sev, 256, "severity.%d", request_data->severity);
    json_object_object_add(increments->others, sev, NEW_INT1);
}

int replace_dots_and_dollars(char *s)
{
    if (s == NULL) return 0;
    int count = 0;
    char c;
    while ((c = *s) != '\0') {
        if (c == '.' || c == '$') {
            *s = '_';
            count++;
        }
        s++;
    }
    return count;
}

int copy_replace_dots_and_dollars(char* buffer, const char *s)
{
    int len = 0;
    if (s != NULL) {
        char c;
        while ((c = *s) != '\0') {
            if (c == '.') {
                char *p = UTF8_DOT;
                *buffer++ = *p++;
                *buffer++ = *p++;
                *buffer++ = *p;
                len += 3;
            } else if (c == '$') {
                char *p = UTF8_CURRENCY;
                *buffer++ = *p++;
                *buffer++ = *p;
                len += 2;
            } else {
                *buffer++ = c;
                len++;
            }
            s++;
        }
    }
    *buffer = '\0';
    return len;
}

int uri_replace_dots_and_dollars(char* buffer, const char *s)
{
    int len = 0;
    if (s != NULL) {
        char c;
        while ((c = *s) != '\0') {
            if (c == '.') {
                char *p = URI_ESCAPED_DOT;
                *buffer++ = *p++;
                *buffer++ = *p++;
                *buffer++ = *p;
                len += 3;
            } else if (c == '$') {
                char *p = URI_ESCAPED_DOLLAR;
                *buffer++ = *p++;
                *buffer++ = *p++;
                *buffer++ = *p;
                len += 3;
            } else {
                *buffer++ = c;
                len++;
            }
            s++;
        }
    }
    *buffer = '\0';
    return len;
}


static char *win1252_to_utf8[128] = {
    /* 0x80 */	  "\u20AC"   ,   // Euro Sign
    /* 0x81 */	  "\uFFFD"   ,   //
    /* 0x82 */	  "\u201A"   ,   // Single Low-9 Quotation Mark
    /* 0x83 */	  "\u0192"   ,   // Latin Small Letter F With Hook
    /* 0x84 */	  "\u201E"   ,   // Double Low-9 Quotation Mark
    /* 0x85 */	  "\u2026"   ,   // Horizontal Ellipsis
    /* 0x86 */	  "\u2020"   ,   // Dagger
    /* 0x87 */	  "\u2021"   ,   // Double Dagger
    /* 0x88 */	  "\u02C6"   ,   // Modifier Letter Circumflex Accent
    /* 0x89 */	  "\u2030"   ,   // Per Mille Sign
    /* 0x8A */	  "\u0160"   ,   // Latin Capital Letter S With Caron
    /* 0x8B */	  "\u2039"   ,   // Single Left-pointing Angle Quotation Mark
    /* 0x8C */	  "\u0152"   ,   // Latin Capital Ligature Oe
    /* 0x8D */	  "\uFFFD"   ,   //
    /* 0x8E */	  "\u017D"   ,   // Latin Capital Letter Z With Caron
    /* 0x8F */	  "\uFFFD"   ,   //
    /* 0x90 */	  "\uFFFD"   ,   //
    /* 0x91 */	  "\u2018"   ,   // Left Single Quotation Mark
    /* 0x92 */	  "\u2019"   ,   // Right Single Quotation Mark
    /* 0x93 */	  "\u201C"   ,   // Left Double Quotation Mark
    /* 0x94 */	  "\u201D"   ,   // Right Double Quotation Mark
    /* 0x95 */	  "\u2022"   ,   // Bullet
    /* 0x96 */	  "\u2013"   ,   // En Dash
    /* 0x97 */	  "\u2014"   ,   // Em Dash
    /* 0x98 */	  "\u02DC"   ,   // Small Tilde
    /* 0x99 */	  "\u2122"   ,   // Trade Mark Sign
    /* 0x9A */	  "\u0161"   ,   // Latin Small Letter S With Caron
    /* 0x9B */	  "\u203A"   ,   // Single Right-pointing Angle Quotation Mark
    /* 0x9C */	  "\u0153"   ,   // Latin Small Ligature Oe
    /* 0x9D */	  "\uFFFD"   ,   //
    /* 0x9E */	  "\u017E"   ,   // Latin Small Letter Z With Caron
    /* 0x9F */	  "\u0178"   ,   // Latin Capital Letter Y With Diaeresis
    /* 0xA0 */	  "\u00A0"   ,   // No-break Space
    /* 0xA1 */	  "\u00A1"   ,   // Inverted Exclamation Mark
    /* 0xA2 */	  "\u00A2"   ,   // Cent Sign
    /* 0xA3 */	  "\u00A3"   ,   // Pound Sign
    /* 0xA4 */	  "\u00A4"   ,   // Currency Sign
    /* 0xA5 */	  "\u00A5"   ,   // Yen Sign
    /* 0xA6 */	  "\u00A6"   ,   // Broken Bar
    /* 0xA7 */	  "\u00A7"   ,   // Section Sign
    /* 0xA8 */	  "\u00A8"   ,   // Diaeresis
    /* 0xA9 */	  "\u00A9"   ,   // Copyright Sign
    /* 0xAA */	  "\u00AA"   ,   // Feminine Ordinal Indicator
    /* 0xAB */	  "\u00AB"   ,   // Left-pointing Double Angle Quotation Mark
    /* 0xAC */	  "\u00AC"   ,   // Not Sign
    /* 0xAD */	  "\u00AD"   ,   // Soft Hyphen
    /* 0xAE */	  "\u00AE"   ,   // Registered Sign
    /* 0xAF */	  "\u00AF"   ,   // Macron
    /* 0xB0 */	  "\u00B0"   ,   // Degree Sign
    /* 0xB1 */	  "\u00B1"   ,   // Plus-minus Sign
    /* 0xB2 */	  "\u00B2"   ,   // Superscript Two
    /* 0xB3 */	  "\u00B3"   ,   // Superscript Three
    /* 0xB4 */	  "\u00B4"   ,   // Acute Accent
    /* 0xB5 */	  "\u00B5"   ,   // Micro Sign
    /* 0xB6 */	  "\u00B6"   ,   // Pilcrow Sign
    /* 0xB7 */	  "\u00B7"   ,   // Middle Dot
    /* 0xB8 */	  "\u00B8"   ,   // Cedilla
    /* 0xB9 */	  "\u00B9"   ,   // Superscript One
    /* 0xBA */	  "\u00BA"   ,   // Masculine Ordinal Indicator
    /* 0xBB */	  "\u00BB"   ,   // Right-pointing Double Angle Quotation Mark
    /* 0xBC */	  "\u00BC"   ,   // Vulgar Fraction One Quarter
    /* 0xBD */	  "\u00BD"   ,   // Vulgar Fraction One Half
    /* 0xBE */	  "\u00BE"   ,   // Vulgar Fraction Three Quarters
    /* 0xBF */	  "\u00BF"   ,   // Inverted Question Mark
    /* 0xC0 */	  "\u00C0"   ,   // Latin Capital Letter A With Grave
    /* 0xC1 */	  "\u00C1"   ,   // Latin Capital Letter A With Acute
    /* 0xC2 */	  "\u00C2"   ,   // Latin Capital Letter A With Circumflex
    /* 0xC3 */	  "\u00C3"   ,   // Latin Capital Letter A With Tilde
    /* 0xC4 */	  "\u00C4"   ,   // Latin Capital Letter A With Diaeresis
    /* 0xC5 */	  "\u00C5"   ,   // Latin Capital Letter A With Ring Above
    /* 0xC6 */	  "\u00C6"   ,   // Latin Capital Letter Ae
    /* 0xC7 */	  "\u00C7"   ,   // Latin Capital Letter C With Cedilla
    /* 0xC8 */	  "\u00C8"   ,   // Latin Capital Letter E With Grave
    /* 0xC9 */	  "\u00C9"   ,   // Latin Capital Letter E With Acute
    /* 0xCA */	  "\u00CA"   ,   // Latin Capital Letter E With Circumflex
    /* 0xCB */	  "\u00CB"   ,   // Latin Capital Letter E With Diaeresis
    /* 0xCC */	  "\u00CC"   ,   // Latin Capital Letter I With Grave
    /* 0xCD */	  "\u00CD"   ,   // Latin Capital Letter I With Acute
    /* 0xCE */	  "\u00CE"   ,   // Latin Capital Letter I With Circumflex
    /* 0xCF */	  "\u00CF"   ,   // Latin Capital Letter I With Diaeresis
    /* 0xD0 */	  "\u00D0"   ,   // Latin Capital Letter Eth
    /* 0xD1 */	  "\u00D1"   ,   // Latin Capital Letter N With Tilde
    /* 0xD2 */	  "\u00D2"   ,   // Latin Capital Letter O With Grave
    /* 0xD3 */	  "\u00D3"   ,   // Latin Capital Letter O With Acute
    /* 0xD4 */	  "\u00D4"   ,   // Latin Capital Letter O With Circumflex
    /* 0xD5 */	  "\u00D5"   ,   // Latin Capital Letter O With Tilde
    /* 0xD6 */	  "\u00D6"   ,   // Latin Capital Letter O With Diaeresis
    /* 0xD7 */	  "\u00D7"   ,   // Multiplication Sign
    /* 0xD8 */	  "\u00D8"   ,   // Latin Capital Letter O With Stroke
    /* 0xD9 */	  "\u00D9"   ,   // Latin Capital Letter U With Grave
    /* 0xDA */	  "\u00DA"   ,   // Latin Capital Letter U With Acute
    /* 0xDB */	  "\u00DB"   ,   // Latin Capital Letter U With Circumflex
    /* 0xDC */	  "\u00DC"   ,   // Latin Capital Letter U With Diaeresis
    /* 0xDD */	  "\u00DD"   ,   // Latin Capital Letter Y With Acute
    /* 0xDE */	  "\u00DE"   ,   // Latin Capital Letter Thorn
    /* 0xDF */	  "\u00DF"   ,   // Latin Small Letter Sharp S
    /* 0xE0 */	  "\u00E0"   ,   // Latin Small Letter A With Grave
    /* 0xE1 */	  "\u00E1"   ,   // Latin Small Letter A With Acute
    /* 0xE2 */	  "\u00E2"   ,   // Latin Small Letter A With Circumflex
    /* 0xE3 */	  "\u00E3"   ,   // Latin Small Letter A With Tilde
    /* 0xE4 */	  "\u00E4"   ,   // Latin Small Letter A With Diaeresis
    /* 0xE5 */	  "\u00E5"   ,   // Latin Small Letter A With Ring Above
    /* 0xE6 */	  "\u00E6"   ,   // Latin Small Letter Ae
    /* 0xE7 */	  "\u00E7"   ,   // Latin Small Letter C With Cedilla
    /* 0xE8 */	  "\u00E8"   ,   // Latin Small Letter E With Grave
    /* 0xE9 */	  "\u00E9"   ,   // Latin Small Letter E With Acute
    /* 0xEA */	  "\u00EA"   ,   // Latin Small Letter E With Circumflex
    /* 0xEB */	  "\u00EB"   ,   // Latin Small Letter E With Diaeresis
    /* 0xEC */	  "\u00EC"   ,   // Latin Small Letter I With Grave
    /* 0xED */	  "\u00ED"   ,   // Latin Small Letter I With Acute
    /* 0xEE */	  "\u00EE"   ,   // Latin Small Letter I With Circumflex
    /* 0xEF */	  "\u00EF"   ,   // Latin Small Letter I With Diaeresis
    /* 0xF0 */	  "\u00F0"   ,   // Latin Small Letter Eth
    /* 0xF1 */	  "\u00F1"   ,   // Latin Small Letter N With Tilde
    /* 0xF2 */	  "\u00F2"   ,   // Latin Small Letter O With Grave
    /* 0xF3 */	  "\u00F3"   ,   // Latin Small Letter O With Acute
    /* 0xF4 */	  "\u00F4"   ,   // Latin Small Letter O With Circumflex
    /* 0xF5 */	  "\u00F5"   ,   // Latin Small Letter O With Tilde
    /* 0xF6 */	  "\u00F6"   ,   // Latin Small Letter O With Diaeresis
    /* 0xF7 */	  "\u00F7"   ,   // Division Sign
    /* 0xF8 */	  "\u00F8"   ,   // Latin Small Letter O With Stroke
    /* 0xF9 */	  "\u00F9"   ,   // Latin Small Letter U With Grave
    /* 0xFA */	  "\u00FA"   ,   // Latin Small Letter U With Acute
    /* 0xFB */	  "\u00FB"   ,   // Latin Small Letter U With Circumflex
    /* 0xFC */	  "\u00FC"   ,   // Latin Small Letter U With Diaeresis
    /* 0xFD */	  "\u00FD"   ,   // Latin Small Letter Y With Acute
    /* 0xFE */	  "\u00FE"   ,   // Latin Small Letter Thorn
    /* 0xFF */	  "\u00FF"   ,   // Latin Small Letter Y With Diaeresis
};

int convert_to_win1252(const char *str, size_t n, char *utf8)
{
    int j = 0;
    for (int i=0; i < n; i++) {
        uint8_t c = str[i];
        if ((c & 0x80) == 0) { // ascii 7bit
            // handle null characters
            if (c)
                utf8[j++] = c;
            else {
                utf8[j++] = '\\';
                utf8[j++] = 'u';
                utf8[j++] = '0';
                utf8[j++] = '0';
                utf8[j++] = '0';
                utf8[j++] = '0';
           }
        } else { // high bit set
            char *t = win1252_to_utf8[c & 0x7F];
            while ( (c = *t++) ) {
                utf8[j++] = c;
            }
        }
    }
    utf8[j] = '\0';
    return j-1;
}

void increments_fill_exceptions(increments_t *increments, json_object *exceptions)
{
    if (exceptions == NULL)
        return;
    int n = json_object_array_length(exceptions);
    if (n == 0)
        return;

    for (int i=0; i<n; i++) {
        json_object* ex_obj = json_object_array_get_idx(exceptions, i);
        const char *ex_str = json_object_get_string(ex_obj);
        size_t n = strlen(ex_str);
        char ex_str_dup[n+12];
        strcpy(ex_str_dup, "exceptions.");
        strcpy(ex_str_dup+11, ex_str);
        int replaced_count = replace_dots_and_dollars(ex_str_dup+11);
        // printf("[D] EXCEPTION: %s\n", ex_str_dup);
        if (replaced_count > 0) {
            json_object* new_ex = json_object_new_string(ex_str_dup+11);
            json_object_array_put_idx(exceptions, i, new_ex);
        }
        json_object_object_add(increments->others, ex_str_dup, NEW_INT1);
    }
}

void increments_fill_js_exception(increments_t *increments, const char *js_exception)
{
    size_t n = strlen(js_exception);
    int l = 14;
    char xbuffer[l+3*n+1];
    strcpy(xbuffer, "js_exceptions.");
    uri_replace_dots_and_dollars(xbuffer+l, js_exception);
    // printf("[D] JS EXCEPTION: %s\n", xbuffer);
    json_object_object_add(increments->others, xbuffer, NEW_INT1);
}

void increments_fill_caller_info(increments_t *increments, json_object *request)
{
    json_object *caller_action_obj;
    if (json_object_object_get_ex(request, "caller_action", &caller_action_obj)) {
        const char *caller_action = json_object_get_string(caller_action_obj);
        if (caller_action == NULL || *caller_action == '\0') return;
        json_object *caller_id_obj;
        if (json_object_object_get_ex(request, "caller_id", &caller_id_obj)) {
            const char *caller_id = json_object_get_string(caller_id_obj);
            if (caller_id == NULL || *caller_id == '\0') return;
            size_t n = strlen(caller_id);
            char app[n], env[n], rid[n];
            if (3 == sscanf(caller_id, "%[^-]-%[^-]-%[^-]", app, env, rid)) {
                size_t app_len = strlen(app);
                size_t action_len = strlen(caller_action);
                char caller_name[4*(app_len + action_len) + 2 + 8];
                strcpy(caller_name, "callers.");
                int real_app_len = copy_replace_dots_and_dollars(caller_name + 8, app);
                caller_name[real_app_len + 8] = '-';
                copy_replace_dots_and_dollars(caller_name + 8 + real_app_len + 1, caller_action);
                // printf("[D] CALLER: %s\n", caller_name);
                json_object_object_add(increments->others, caller_name, NEW_INT1);
            }
        }
    }
}

void increments_add(increments_t *stored_increments, increments_t* increments)
{
    stored_increments->backend_request_count += increments->backend_request_count;
    stored_increments->page_request_count += increments->page_request_count;
    stored_increments->ajax_request_count += increments->ajax_request_count;
    for (size_t i=0; i<=last_resource_index; i++) {
        metric_pair_t *stored = &(stored_increments->metrics[i]);
        metric_pair_t *addend = &(increments->metrics[i]);
        stored->val += addend->val;
        stored->val_squared += addend->val_squared;
    }
    json_object_object_foreach(increments->others, key, value) {
        json_object *stored_obj = NULL;
        json_object *new_obj = NULL;
        bool perform_addition = json_object_object_get_ex(stored_increments->others, key, &stored_obj);
        enum json_type type = json_object_get_type(value);
        switch (type) {
        case json_type_double: {
            double addend = json_object_get_double(value);
            if (perform_addition) {
                double stored = json_object_get_double(stored_obj);
                new_obj = json_object_new_double(stored + addend);
            } else {
                new_obj = json_object_new_double(addend);
            }
            break;
        }
        case json_type_int: {
            int addend = json_object_get_int(value);
            if (perform_addition) {
                int stored = json_object_get_int(stored_obj);
                new_obj = json_object_new_int(stored + addend);
            } else {
                new_obj = json_object_new_int(addend);
            }
            break;
        }
        default:
            fprintf(stderr, "[E] unknown increment type: %s, for key: %s\n", json_type_to_name(type), key);
            dump_json_object(stderr, increments->others);
        }
        if (new_obj) {
            json_object_object_add(stored_increments->others, key, new_obj);
        }
    }
}

const char* append_to_json_string(json_object **jobj, const char* old_str, const char* add_str)
{
    int old_len = strlen(old_str);
    int add_len = strlen(add_str);
    int new_len = old_len + add_len;
    char new_str_value[new_len+1];
    memcpy(new_str_value, old_str, old_len);
    memcpy(new_str_value + old_len, add_str, add_len);
    new_str_value[new_len] = '\0';
    json_object_put(*jobj);
    *jobj = json_object_new_string(new_str_value);
    return json_object_get_string(*jobj);
}

int dump_module_name(const char* key, void *module, void *arg)
{
    printf("[D] module: %s\n", (char*)module);
    return 0;
}

void dump_metrics(metric_pair_t *metrics)
{
    for (size_t i=0; i<=last_resource_index; i++) {
        if (metrics[i].val > 0) {
            printf("[D] %s:%f:%f\n", int_to_resource[i], metrics[i].val, metrics[i].val_squared);
        }
    }
}

int dump_increments(const char *key, void *total, void *arg)
{
    puts("[D] ------------------------------------------------");
    printf("[D] action: %s\n", key);
    increments_t* increments = total;
    printf("[D] backend requests: %zu\n", increments->backend_request_count);
    printf("[D] page requests: %zu\n", increments->page_request_count);
    printf("[D] ajax requests: %zu\n", increments->ajax_request_count);
    dump_metrics(increments->metrics);
    dump_json_object(stdout, increments->others);
    return 0;
}

void processor_dump_state(processor_state_t *self)
{
    puts("[D] ================================================");
    printf("[D] db_name: %s\n", self->db_name);
    printf("[D] processed requests: %zu\n", self->request_count);
    zhash_foreach(self->modules, dump_module_name, NULL);
    zhash_foreach(self->totals, dump_increments, NULL);
    zhash_foreach(self->minutes, dump_increments, NULL);
}

int processor_dump_state_from_zhash(const char* db_name, void* processor, void* arg)
{
    assert(!strcmp(((processor_state_t*)processor)->db_name, db_name));
    processor_dump_state(processor);
    return 0;
}

bson_t* increments_to_bson(const char* namespace, increments_t* increments)
{
    // dump_increments(namespace, increments, NULL);

    bson_t *incs = bson_new();
    if (increments->backend_request_count)
        bson_append_int32(incs, "count", 5, increments->backend_request_count);
    if (increments->page_request_count)
        bson_append_int32(incs, "page_count", 10, increments->page_request_count);
    if (increments->ajax_request_count)
        bson_append_int32(incs, "ajax_count", 10, increments->ajax_request_count);

    for (size_t i=0; i<=last_resource_index; i++) {
        double val = increments->metrics[i].val;
        if (val > 0) {
            const char *name = int_to_resource[i];
            bson_append_double(incs, name, strlen(name), val);
            const char *name_sq = int_to_resource_sq[i];
            bson_append_double(incs, name_sq, strlen(name_sq), increments->metrics[i].val_squared);
        }
    }

    json_object_object_foreach(increments->others, key, value_obj) {
        size_t n = strlen(key);
        enum json_type type = json_object_get_type(value_obj);
        switch (type) {
        case json_type_int:
            bson_append_int32(incs, key, n, json_object_get_int(value_obj));
            break;
        case json_type_double:
            bson_append_double(incs, key, n, json_object_get_double(value_obj));
            break;
        default:
            fprintf(stderr, "[E] unsupported json type in json to bson conversion: %s, key: %s\n", json_type_to_name(type), key);
        }
    }

    bson_t *document = bson_new();
    bson_append_document(document, "$inc", 4, incs);

    // size_t n;
    // char* bs = bson_as_json(document, &n);
    // printf("[D] document. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_destroy(incs);

    return document;
}

int minutes_add_increments(const char* namespace, void* data, void* arg)
{
    collection_update_callback_t *cb = arg;
    mongoc_collection_t *collection = cb->collection;
    const char *db_name = cb->db_name;
    increments_t* increments = data;

    int minute = 0;
    char* p = (char*) namespace;
    while (isdigit(*p)) {
        minute *= 10;
        minute += *(p++) - '0';
    }
    p++;

    bson_t *selector = bson_new();
    assert( bson_append_utf8(selector, "page", 4, p, strlen(p)) );
    assert( bson_append_int32(selector, "minute", 6, minute ) );

    // size_t n;
    // char* bs = bson_as_json(selector, &n);
    // printf("[D] selector. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_t *document = increments_to_bson(namespace, increments);
    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying minutes update operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] update failed for %s on minutes: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(selector);
    bson_destroy(document);
    return 0;
}

int totals_add_increments(const char* namespace, void* data, void* arg)
{
    collection_update_callback_t *cb = arg;
    mongoc_collection_t *collection = cb->collection;
    const char *db_name = cb->db_name;
    increments_t* increments = data;
    assert(increments);

    bson_t *selector = bson_new();
    assert( bson_append_utf8(selector, "page", 4, namespace, strlen(namespace)) );

    // size_t n;
    // char* bs = bson_as_json(selector, &n);
    // printf("[D] selector. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_t *document = increments_to_bson(namespace, increments);
    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying totals update operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] update failed for %s on totals: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }

    bson_destroy(selector);
    bson_destroy(document);
    return 0;
}

int quants_add_quants(const char* namespace, void* data, void* arg)
{
    collection_update_callback_t *cb = arg;
    mongoc_collection_t *collection = cb->collection;
    const char *db_name = cb->db_name;

    // extract keys from namespace: kind-quant-page
    char* p = (char*) namespace;
    char kind[2];
    kind[0] = *(p++);
    kind[1] = '\0';

    p++; // skip '-'
    size_t quant = 0;
    while (isdigit(*p)) {
        quant *= 10;
        quant += *(p++) - '0';
    }
    p++; // skip '-'

    bson_t *selector = bson_new();
    bson_append_utf8(selector, "page", 4, p, strlen(p));
    bson_append_utf8(selector, "kind", 4, kind, 1);
    bson_append_int32(selector, "quant", 5, quant);

    // size_t n;
    // char* bs = bson_as_json(selector, &n);
    // printf("[D] selector. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_t *incs = bson_new();
    size_t *quants = data;
    for (int i=0; i <= last_resource_index; i++) {
        if (quants[i] > 0) {
            const char *resource = i2r(i);
            bson_append_int32(incs, resource, -1, quants[i]);
        }
    }
    bson_t *document = bson_new();
    bson_append_document(document, "$inc", 4, incs);

    // size_t n; char*
    // bs = bson_as_json(document, &n);
    // printf("[D] document. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying quants update operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] update failed for %s on quants: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(selector);
    bson_destroy(incs);
    bson_destroy(document);
    return 0;
}

void ensure_known_database(mongoc_client_t *client, const char* db_name)
{
    mongoc_collection_t *meta_collection = mongoc_client_get_collection(client, "logjam-global", "metadata");
    bson_t *selector = bson_new();
    assert(bson_append_utf8(selector, "name", 4, "databases", 9));

    bson_t *document = bson_new();
    bson_t *sub_doc = bson_new();
    bson_append_utf8(sub_doc, "value", 5, db_name, -1);
    bson_append_document(document, "$addToSet", 9, sub_doc);

    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES+3; // try harder than for normal updates
    retry:
        if (!mongoc_collection_update(meta_collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying update on logjam-global: %s\n", db_name);
                goto retry;
            } else {
                fprintf(stderr, "[E] update failed on logjam-global: (%d) %s\n", error.code, error.message);
            }
        }
    }

    bson_destroy(selector);
    bson_destroy(document);
    bson_destroy(sub_doc);

    mongoc_collection_destroy(meta_collection);
}

stats_collections_t *stats_collections_new(mongoc_client_t* client, const char* db_name)
{
    stats_collections_t *collections = malloc(sizeof(stats_collections_t));
    assert(collections);
    memset(collections, 0, sizeof(stats_collections_t));

    if (dryrun) return collections;

    collections->totals = mongoc_client_get_collection(client, db_name, "totals");
    collections->minutes = mongoc_client_get_collection(client, db_name, "minutes");
    collections->quants = mongoc_client_get_collection(client, db_name, "quants");

    return collections;
}

void destroy_stats_collections(stats_collections_t* collections)
{
    if (collections->totals != NULL)
        mongoc_collection_destroy(collections->totals);
    if (collections->minutes != NULL)
        mongoc_collection_destroy(collections->minutes);
    if (collections->quants != NULL)
        mongoc_collection_destroy(collections->quants);
    free(collections);
}

stats_collections_t *stats_updater_get_collections(stats_updater_state_t *self, const char* db_name, stream_info_t *stream_info)
{
    stats_collections_t *collections = zhash_lookup(self->stats_collections, db_name);
    if (collections == NULL) {
        mongoc_client_t *mongo_client = self->mongo_clients[stream_info->db];
        // ensure_known_database(mongo_client, db_name);
        collections = stats_collections_new(mongo_client, db_name);
        assert(collections);
        zhash_insert(self->stats_collections, db_name, collections);
        zhash_freefn(self->stats_collections, db_name, (zhash_free_fn*)destroy_stats_collections);
    }
    return collections;
}


const char* processor_setup_page(processor_state_t *self, json_object *request)
{
    json_object *page_obj = NULL;
    if (json_object_object_get_ex(request, "action", &page_obj)) {
        json_object_get(page_obj);
        json_object_object_del(request, "action");
    } else {
        page_obj = json_object_new_string("Unknown#unknown_method");
    }

    const char *page_str = json_object_get_string(page_obj);

    if (!strchr(page_str, '#'))
        page_str = append_to_json_string(&page_obj, page_str, "#unknown_method");
    else if (page_str[strlen(page_str)-1] == '#')
        page_str = append_to_json_string(&page_obj, page_str, "unknown_method");

    json_object_object_add(request, "page", page_obj);

    return page_str;
}

const char* processor_setup_module(processor_state_t *self, const char *page)
{
    int max_mod_len = strlen(page);
    char module_str[max_mod_len+1];
    char *mod_ptr = strchr(page, ':');
    strcpy(module_str, "::");
    if (mod_ptr != NULL){
        if (mod_ptr != page) {
            int mod_len = mod_ptr - page;
            memcpy(module_str+2, page, mod_len);
            module_str[mod_len+2] = '\0';
        }
    } else {
        char *action_ptr = strchr(page, '#');
        if (action_ptr != NULL) {
            int mod_len = action_ptr - page;
            memcpy(module_str+2, page, mod_len);
            module_str[mod_len+2] = '\0';
        }
    }
    char *module = zhash_lookup(self->modules, module_str);
    if (module == NULL) {
        module = strdup(module_str);
        int rc = zhash_insert(self->modules, module, module);
        assert(rc == 0);
        zhash_freefn(self->modules, module, free);
    }
    // printf("[D] page: %s\n", page);
    // printf("[D] module: %s\n", module);
    return module;
}

int processor_setup_response_code(processor_state_t *self, json_object *request)
{
    json_object *code_obj = NULL;
    int response_code = 500;
    if (json_object_object_get_ex(request, "code", &code_obj)) {
        response_code = json_object_get_int(code_obj);
        json_object_object_del(request, "code");
    }
    json_object_object_add(request, "response_code", json_object_new_int(response_code));
    // printf("[D] response_code: %d\n", response_code);
    return response_code;
}

double processor_setup_time(processor_state_t *self, json_object *request, const char *time_name)
{
    // TODO: might be better to drop requests without total_time
    double total_time;
    json_object *total_time_obj = NULL;
    if (json_object_object_get_ex(request, time_name, &total_time_obj)) {
        total_time = json_object_get_double(total_time_obj);
        if (total_time == 0.0) {
            total_time = 1.0;
            total_time_obj = json_object_new_double(total_time);
            json_object_object_add(request, time_name, total_time_obj);
        }
    } else {
        total_time = 1.0;
        total_time_obj = json_object_new_double(total_time);
        json_object_object_add(request, time_name, total_time_obj);
    }
    // printf("[D] %s: %f\n", time_name, total_time);
    return total_time;
}

int extract_severity_from_lines_object(json_object* lines)
{
    int log_level = -1;
    if (lines != NULL && json_object_get_type(lines) == json_type_array) {
        int array_len = json_object_array_length(lines);
        for (int i=0; i<array_len; i++) {
            json_object *line = json_object_array_get_idx(lines, i);
            if (line && json_object_get_type(line) == json_type_array) {
                json_object *level = json_object_array_get_idx(line, 0);
                if (level) {
                    int new_level = json_object_get_int(level);
                    if (new_level > log_level) {
                        log_level = new_level;
                    }
                }
            }
        }
    }
    // protect against unknown log levels
    return (log_level > 5) ? -1 : log_level;
}

int processor_setup_severity(processor_state_t *self, json_object *request)
{
    int severity = 1;
    json_object *severity_obj;
    if (json_object_object_get_ex(request, "severity", &severity_obj)) {
        severity = json_object_get_int(severity_obj);
    } else {
        json_object *lines_obj;
        if (json_object_object_get_ex(request, "lines", &lines_obj)) {
            int extracted_severity = extract_severity_from_lines_object(lines_obj);
            if (extracted_severity != -1) {
                severity = extracted_severity;
            }
        }
        severity_obj = json_object_new_int(severity);
        json_object_object_add(request, "severity", severity_obj);
    }
    return severity;
    // printf("[D] severity: %d\n\n", severity);
}

int processor_setup_minute(processor_state_t *self, json_object *request)
{
    // we know that started_at data is valid since we already checked that
    // when determining which processor to call
    int minute = 0;
    json_object *started_at_obj = NULL;
    if (json_object_object_get_ex(request, "started_at", &started_at_obj)) {
        const char *started_at = json_object_get_string(started_at_obj);
        char hours[3] = {started_at[11], started_at[12], '\0'};
        char minutes[3] = {started_at[14], started_at[15], '\0'};
        minute = 60 * atoi(hours) + atoi(minutes);
    }
    json_object *minute_obj = json_object_new_int(minute);
    json_object_object_add(request, "minute", minute_obj);
    // printf("[D] minute: %d\n", minute);
    return minute;
}

void processor_setup_other_time(processor_state_t *self, json_object *request, double total_time)
{
    double other_time = total_time;
    for (size_t i = 0; i <= last_other_time_resource_index; i++) {
        json_object *time_val;
        if (json_object_object_get_ex(request, other_time_resources[i], &time_val)) {
            double v = json_object_get_double(time_val);
            other_time -= v;
        }
    }
    json_object_object_add(request, "other_time", json_object_new_double(other_time));
    // printf("[D] other_time: %f\n", other_time);
}

void processor_setup_allocated_memory(processor_state_t *self, json_object *request)
{
    json_object *allocated_memory_obj;
    if (json_object_object_get_ex(request, "allocated_memory", &allocated_memory_obj))
        return;
    json_object *allocated_objects_obj;
    if (!json_object_object_get_ex(request, "allocated_objects", &allocated_objects_obj))
        return;
    json_object *allocated_bytes_obj;
    if (json_object_object_get_ex(request, "allocated_bytes", &allocated_bytes_obj)) {
        long allocated_objects = json_object_get_int64(allocated_objects_obj);
        long allocated_bytes = json_object_get_int64(allocated_bytes_obj);
        // assume 64bit ruby
        long allocated_memory = allocated_bytes + allocated_objects * 40;
        json_object_object_add(request, "allocated_memory", json_object_new_int64(allocated_memory));
        // printf("[D] allocated memory: %lu\n", allocated_memory);
    }
}

int processor_setup_heap_growth(processor_state_t *self, json_object *request)
{
    json_object *heap_growth_obj = NULL;
    int heap_growth = 0;
    if (json_object_object_get_ex(request, "heap_growth", &heap_growth_obj)) {
        heap_growth = json_object_get_int(heap_growth_obj);
    }
    // printf("[D] heap_growth: %d\n", heap_growth);
    return heap_growth;
}

json_object* processor_setup_exceptions(processor_state_t *self, json_object *request)
{
    json_object* exceptions;
    if (json_object_object_get_ex(request, "exceptions", &exceptions)) {
        int num_ex = json_object_array_length(exceptions);
        if (num_ex == 0) {
            json_object_object_del(request, "exceptions");
            return NULL;
        }
    }
    return exceptions;
}

void processor_add_totals(processor_state_t *self, const char* namespace, increments_t *increments)
{
    increments_t *stored_increments = zhash_lookup(self->totals, namespace);
    if (stored_increments) {
        increments_add(stored_increments, increments);
    } else {
        increments_t *duped_increments = increments_clone(increments);
        int rc = zhash_insert(self->totals, namespace, duped_increments);
        assert(rc == 0);
        assert(zhash_freefn(self->totals, namespace, increments_destroy));
    }
}

void processor_add_minutes(processor_state_t *self, const char* namespace, size_t minute, increments_t *increments)
{
    char key[2000];
    snprintf(key, 2000, "%lu-%s", minute, namespace);
    increments_t *stored_increments = zhash_lookup(self->minutes, key);
    if (stored_increments) {
        increments_add(stored_increments, increments);
    } else {
        increments_t *duped_increments = increments_clone(increments);
        int rc = zhash_insert(self->minutes, key, duped_increments);
        assert(rc == 0);
        assert(zhash_freefn(self->minutes, key, increments_destroy));
    }
}

int add_quant_to_quants_hash(const char* key, void* data, void *arg)
{
    hash_pair_t *p = arg;
    size_t *stored = zhash_lookup(p->target, key);
    if (stored != NULL) {
        for (int i=0; i <= last_resource_index; i++) {
            stored[i] += ((size_t*)data)[i];
        }
    } else {
        zhash_insert(p->target, key, data);
        zhash_freefn(p->target, key, free);
        zhash_freefn(p->source, key, NULL);
    }
    return 0;
}

void combine_quants(zhash_t *target, zhash_t *source)
{
    hash_pair_t hash_pair;
    hash_pair.source = source;
    hash_pair.target = target;
    zhash_foreach(source, add_quant_to_quants_hash, &hash_pair);
}

void add_quant(const char* namespace, size_t resource_idx, char kind, size_t quant, zhash_t* quants)
{
    char key[2000];
    sprintf(key, "%c-%zu-%s", kind, quant, namespace);
    // printf("[D] QUANT-KEY: %s\n", key);
    size_t *stored = zhash_lookup(quants, key);
    if (stored == NULL) {
        stored = malloc(QUANTS_ARRAY_SIZE);
        memset(stored, 0, QUANTS_ARRAY_SIZE);
        zhash_insert(quants, key, stored);
        zhash_freefn(quants, key, free);
    }
    stored[resource_idx]++;
}

void processor_add_quants(processor_state_t *self, const char* namespace, increments_t *increments)
{
    for (int i=0; i<=last_resource_index; i++){
        double val = increments->metrics[i].val;
        if (val > 0) {
            char kind;
            double d;
            if (i <= last_time_resource_index) {
                kind = 't';
                d = 100.0;
            } else if (i == allocated_objects_index) {
                kind = 'm';
                d = 10000.0;
            } else if (i == allocated_bytes_index) {
                kind = 'm';
                d = 100000.0;
            } else {
                continue;
            }
            size_t x = (ceil(floor(val/d))+1) * d;
            add_quant(namespace, i, kind, x, self->quants);
            add_quant("all_pages", i, kind, x, self->quants);
        }
    }
}

bool interesting_request(request_data_t *request_data, json_object *request, stream_info_t* info)
{
    int time_threshold = info ? info->import_threshold : global_total_time_import_threshold;
    if (request_data->total_time > time_threshold)
        return true;
    if (request_data->severity > 1)
        return true;
    if (request_data->response_code >= 400)
        return true;
    if (request_data->exceptions != NULL)
        return true;
    if (request_data->heap_growth > 0)
        return true;
    if (info == NULL)
        return false;
    for (int i=0; i<info->module_threshold_count; i++) {
        if (!strcmp(request_data->module+2, info->module_thresholds[i].name)) {
            if (request_data->total_time > info->module_thresholds[i].value) {
                // printf("[D] INTERESTING: %s: %f\n", request_data->module+2, request_data->total_time);
                return true;
            } else
                return false;
        }
    }
    return false;
}

int ignore_request(json_object *request, stream_info_t* info)
{
    int rc = 0;
    json_object *req_info;
    if (json_object_object_get_ex(request, "request_info", &req_info)) {
        json_object *url_obj;
        if (json_object_object_get_ex(req_info, "url", &url_obj)) {
            const char *url = json_object_get_string(url_obj);
            const char *prefix = info ? info->ignored_request_prefix : global_ignored_request_prefix;
            if (prefix != NULL && strstr(url, prefix) == url) {
                rc = 1;
            }
        }
    }
    return rc;
}

void processor_add_request(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    if (ignore_request(request, self->stream_info)) return;

    // dump_json_object(stdout, request);
    request_data_t request_data;
    request_data.page = processor_setup_page(self, request);
    request_data.module = processor_setup_module(self, request_data.page);
    request_data.response_code = processor_setup_response_code(self, request);
    request_data.severity = processor_setup_severity(self, request);
    request_data.minute = processor_setup_minute(self, request);
    request_data.total_time = processor_setup_time(self, request, "total_time");

    request_data.exceptions = processor_setup_exceptions(self, request);
    processor_setup_other_time(self, request, request_data.total_time);
    processor_setup_allocated_memory(self, request);
    request_data.heap_growth = processor_setup_heap_growth(self, request);

    increments_t* increments = increments_new();
    increments->backend_request_count = 1;
    increments_fill_metrics(increments, request);
    increments_fill_apdex(increments, &request_data);
    increments_fill_response_code(increments, &request_data);
    increments_fill_severity(increments, &request_data);
    increments_fill_caller_info(increments, request);
    increments_fill_exceptions(increments, request_data.exceptions);

    processor_add_totals(self, request_data.page, increments);
    processor_add_totals(self, request_data.module, increments);
    processor_add_totals(self, "all_pages", increments);

    processor_add_minutes(self, request_data.page, request_data.minute, increments);
    processor_add_minutes(self, request_data.module, request_data.minute, increments);
    processor_add_minutes(self, "all_pages", request_data.minute, increments);

    processor_add_quants(self, request_data.page, increments);

    increments_destroy(increments);
    // dump_json_object(stdout, request);
    // if (self->request_count % 100 == 0) {
    //     processor_dump_state(self);
    // }
    if (interesting_request(&request_data, request, self->stream_info)) {
        json_object_get(request);
        zmsg_t *msg = zmsg_new();
        zmsg_addstr(msg, self->db_name);
        zmsg_addstr(msg, "r");
        zmsg_addstr(msg, request_data.module);
        zmsg_addmem(msg, &request, sizeof(json_object*));
        zmsg_addmem(msg, &self->stream_info, sizeof(stream_info_t*));
        if (!output_socket_ready(pstate->push_socket, 0)) {
            fprintf(stderr, "[W] parser [%zu]: push socket not ready\n", pstate->id);
        }
        zmsg_send(&msg, pstate->push_socket);
    }
}

char* extract_page_for_jse(json_object *request)
{
    json_object *page_obj = NULL;
    if (json_object_object_get_ex(request, "logjam_action", &page_obj)) {
        page_obj = json_object_new_string(json_object_get_string(page_obj));
    } else {
        page_obj = json_object_new_string("Unknown#unknown_method");
    }

    const char *page_str = json_object_get_string(page_obj);

    if (!strchr(page_str, '#'))
        page_str = append_to_json_string(&page_obj, page_str, "#unknown_method");
    else if (page_str[strlen(page_str)-1] == '#')
        page_str = append_to_json_string(&page_obj, page_str, "unknown_method");

    char *page = strdup(page_str);
    json_object_put(page_obj);
    return page;
}

char* exctract_key_from_jse_description(json_object *request)
{
    json_object *description_obj = NULL;
    const char *description;
    if (json_object_object_get_ex(request, "description", &description_obj)) {
        description = json_object_get_string(description_obj);
    } else {
        description = "unknown_exception";
    }
    char *result = strdup(description);
    return result;
}

void processor_add_js_exception(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    char *page = extract_page_for_jse(request);
    char *js_exception = exctract_key_from_jse_description(request);

    if (strlen(js_exception) == 0) {
        fprintf(stderr, "[E] could not extract js_exception from request. ignoring.\n");
        dump_json_object(stderr, request);
        free(page);
        free(js_exception);
        return;
    }

    int minute = processor_setup_minute(self, request);
    const char *module = processor_setup_module(self, page);

    increments_t* increments = increments_new();
    increments_fill_js_exception(increments, js_exception);

    processor_add_totals(self, "all_pages", increments);
    processor_add_minutes(self, "all_pages", minute, increments);

    if (strstr(page, "#unknown_method") == NULL) {
        processor_add_totals(self, page, increments);
        processor_add_minutes(self, page, minute, increments);
    }

    if (strcmp(module, "Unknown") != 0) {
        processor_add_totals(self, module, increments);
        processor_add_minutes(self, module, minute, increments);
    }

    increments_destroy(increments);
    free(page);
    free(js_exception);

    json_object_get(request);
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, self->db_name);
    zmsg_addstr(msg, "j");
    zmsg_addstr(msg, module);
    zmsg_addmem(msg, &request, sizeof(json_object*));
    zmsg_addmem(msg, &self->stream_info, sizeof(stream_info_t*));
    zmsg_send(&msg, pstate->push_socket);
}

void processor_add_event(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    processor_setup_minute(self, request);
    json_object_get(request);
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, self->db_name);
    zmsg_addstr(msg, "e");
    zmsg_addstr(msg, "");
    zmsg_addmem(msg, &request, sizeof(json_object*));
    zmsg_addmem(msg, &self->stream_info, sizeof(stream_info_t*));
    zmsg_send(&msg, pstate->push_socket);
}

void processor_add_frontend_data(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    return;

    request_data_t request_data;
    request_data.page = processor_setup_page(self, request);
    request_data.module = processor_setup_module(self, request_data.page);
    request_data.minute = processor_setup_minute(self, request);
    request_data.total_time = processor_setup_time(self, request, "page_time");

    // TODO: revisit when switching to percentiles
    if (request_data.total_time > 300000) {
        fprintf(stderr, "[W] dropped request data with nonsensical page_time\n");
        dump_json_object(stderr, request);
        return;
    }

    increments_t* increments = increments_new();
    increments->page_request_count = 1;
    increments_fill_metrics(increments, request);
    increments_fill_frontend_apdex(increments, request_data.total_time);

    processor_add_totals(self, request_data.page, increments);
    processor_add_totals(self, request_data.module, increments);
    processor_add_totals(self, "all_pages", increments);

    processor_add_minutes(self, request_data.page, request_data.minute, increments);
    processor_add_minutes(self, request_data.module, request_data.minute, increments);
    processor_add_minutes(self, "all_pages", request_data.minute, increments);

    processor_add_quants(self, request_data.page, increments);

    // dump_increments("add_frontend_data", increments, NULL);

    increments_destroy(increments);

    // TODO: store interesting requests
}

void processor_add_ajax_data(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    return;

    // dump_json_object(stdout, request);
    // if (self->request_count % 100 == 0) {
    //     processor_dump_state(self);
    // }

    request_data_t request_data;
    request_data.page = processor_setup_page(self, request);
    request_data.module = processor_setup_module(self, request_data.page);
    request_data.minute = processor_setup_minute(self, request);
    request_data.total_time = processor_setup_time(self, request, "ajax_time");

    // TODO: revisit when switching to percentiles
    if (request_data.total_time > 60000) {
        fprintf(stderr, "[W] dropped request data with nonsensical ajax_time\n");
        dump_json_object(stderr, request);
        return;
    }

    increments_t* increments = increments_new();
    increments->ajax_request_count = 1;
    increments_fill_metrics(increments, request);
    increments_fill_frontend_apdex(increments, request_data.total_time);

    processor_add_totals(self, request_data.page, increments);
    processor_add_totals(self, request_data.module, increments);
    processor_add_totals(self, "all_pages", increments);

    processor_add_minutes(self, request_data.page, request_data.minute, increments);
    processor_add_minutes(self, request_data.module, request_data.minute, increments);
    processor_add_minutes(self, "all_pages", request_data.minute, increments);

    processor_add_quants(self, request_data.page, increments);

    // dump_increments("add_ajax_data", increments, NULL);

    increments_destroy(increments);

    // TODO: store interesting requests
}

int processor_publish_totals(const char* db_name, void *processor, void *live_stream_socket)
{
    processor_state_t *self = processor;
    if (zhash_size(self->modules) == 0) return 0;

    stream_info_t *stream_info = self->stream_info;
    size_t n = stream_info->app_len + 1 + stream_info->env_len;

    zlist_t *modules = zhash_keys(self->modules);
    zlist_push(modules, "all_pages");
    const char* module = zlist_first(modules);
    while (module != NULL) {
        const char *namespace = module;
        // skip :: at the beginning of module
        while (*module == ':') module++;
        size_t m = strlen(module);
        char key[n + m + 3];
        sprintf(key, "%s-%s,%s", stream_info->app, stream_info->env, module);
        // TODO: change this crap in the live stream publisher
        // tolower is unsafe and not really necessary
        for (char *p = key; *p; ++p) *p = tolower(*p);

        // printf("[D] publishing totals for db: %s, module: %s, key: %s\n", db_name, module, key);
        increments_t *incs = zhash_lookup(self->totals, namespace);
        if (incs) {
            json_object *json = json_object_new_object();
            json_object_object_add(json, "count", json_object_new_int(incs->backend_request_count));
            json_object_object_add(json, "page_count", json_object_new_int(incs->page_request_count));
            json_object_object_add(json, "ajax_count", json_object_new_int(incs->ajax_request_count));
            increments_add_metrics_to_json(incs, json);
            const char* json_str = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);

            live_stream_publish(live_stream_socket, key, json_str);

            json_object_put(json);
        } else {
            fprintf(stderr, "[E] missing increments for db: %s, module: %s, key: %s\n", db_name, module, key);
        }
        module = zlist_next(modules);
    }
    zlist_destroy(&modules);
    return 0;
}


void parse_msg_and_forward_interesting_requests(zmsg_t *msg, parser_state_t *parser_state)
{
    // zmsg_dump(msg);
    if (zmsg_size(msg) < 3) {
        fprintf(stderr, "[E] parser received incomplete message\n");
        my_zmsg_fprint(msg, "[E] FRAME=", stderr);
    }
    zframe_t *stream  = zmsg_first(msg);
    zframe_t *topic   = zmsg_next(msg);
    zframe_t *body    = zmsg_next(msg);
    json_object *request = parse_json_body(body, parser_state->tokener);
    if (request != NULL) {
        char *topic_str = (char*) zframe_data(topic);
        int n = zframe_size(topic);
        processor_state_t *processor = processor_create(stream, parser_state, request);

        if (processor == NULL) {
            fprintf(stderr, "[E] could not create processor\n");
            my_zmsg_fprint(msg, "[E] FRAME=", stderr);
            return;
        }
        processor->request_count++;

        if (n >= 4 && !strncmp("logs", topic_str, 4))
            processor_add_request(processor, parser_state, request);
        else if (n >= 10 && !strncmp("javascript", topic_str, 10))
            processor_add_js_exception(processor, parser_state, request);
        else if (n >= 6 && !strncmp("events", topic_str, 6))
            processor_add_event(processor, parser_state, request);
        else if (n >= 13 && !strncmp("frontend.page", topic_str, 13))
            processor_add_frontend_data(processor, parser_state, request);
        else if (n >= 13 && !strncmp("frontend.ajax", topic_str, 13))
            processor_add_ajax_data(processor, parser_state, request);
        else {
            fprintf(stderr, "[W] unknown topic key\n");
            my_zmsg_fprint(msg, "[E] FRAME=", stderr);
        }
        json_object_put(request);
    } else {
        fprintf(stderr, "[E] parse error\n");
        my_zmsg_fprint(msg, "[E] MSGFRAME=", stderr);
    }
}

zhash_t* processor_hash_new()
{
    zhash_t *hash = zhash_new();
    assert(hash);
    return hash;
}

void parser(void *args, zctx_t *ctx, void *pipe)
{
    parser_state_t state;
    state.id = (size_t)args;
    state.parsed_msgs_count = 0;
    state.controller_socket = pipe;
    state.pull_socket = parser_pull_socket_new(ctx);
    state.push_socket = parser_push_socket_new(ctx);
    state.indexer_socket = parser_indexer_socket_new(ctx);
    assert( state.tokener = json_tokener_new() );
    state.processors = processor_hash_new();

    zpoller_t *poller = zpoller_new(state.controller_socket, state.pull_socket, NULL);
    assert(poller);

    while (!zctx_interrupted) {
        // -1 == block until something is readable
        void *socket = zpoller_wait(poller, -1);
        zmsg_t *msg = NULL;
        if (socket == state.controller_socket) {
            // tick
            if (state.parsed_msgs_count)
                printf("[I] parser [%zu]: tick (%zu messages)\n", state.id, state.parsed_msgs_count);
            msg = zmsg_recv(state.controller_socket);
            zmsg_t *answer = zmsg_new();
            zmsg_addmem(answer, &state.processors, sizeof(zhash_t*));
            zmsg_addmem(answer, &state.parsed_msgs_count, sizeof(size_t));
            zmsg_send(&answer, state.controller_socket);
            state.parsed_msgs_count = 0;
            state.processors = processor_hash_new();
        } else if (socket == state.pull_socket) {
            msg = zmsg_recv(state.pull_socket);
            if (msg != NULL) {
                state.parsed_msgs_count++;
                parse_msg_and_forward_interesting_requests(msg, &state);
            }
        } else {
            // interrupted
            break;
        }
        zmsg_destroy(&msg);
    }
    printf("[I] parser [%zu]: terminated\n", state.id);
}

void extract_parser_state(zmsg_t* msg, zhash_t **processors, size_t *parsed_msgs_count)
{
    zframe_t *first = zmsg_first(msg);
    zframe_t *second = zmsg_next(msg);
    assert(zframe_size(first) == sizeof(zhash_t*));
    memcpy(&*processors, zframe_data(first), sizeof(zhash_t*));
    assert(zframe_size(second) == sizeof(size_t));
    memcpy(parsed_msgs_count, zframe_data(second), sizeof(size_t));
}

void extract_processor_state(zmsg_t* msg, processor_state_t **processor, size_t *request_count)
{
    zframe_t *first = zmsg_first(msg);
    zframe_t *second = zmsg_next(msg);
    assert(zframe_size(first) == sizeof(zhash_t*));
    memcpy(&*processor, zframe_data(first), sizeof(processor_state_t*));
    assert(zframe_size(second) == sizeof(size_t));
    memcpy(request_count, zframe_data(second), sizeof(size_t));
}

int mongo_client_ping(mongoc_client_t *client)
{
    int available = 1;
#if USE_PINGS == 1
    bson_t ping;
    bson_init(&ping);
    bson_append_int32(&ping, "ping", 4, 1);

    mongoc_database_t *database = mongoc_client_get_database(client, "logjam-global");
    mongoc_cursor_t *cursor = mongoc_database_command(database, 0, 0, 1, 0, &ping, NULL, NULL);

    const bson_t *reply;
    bson_error_t error;
    if (mongoc_cursor_next(cursor, &reply)) {
        available = 0;
        // char *str = bson_as_json(reply, NULL);
        // fprintf(stdout, "D %s\n", str);
        // bson_free(str);
    } else if (mongoc_cursor_error(cursor, &error)) {
        fprintf(stderr, "[E] ping failure: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(&ping);
    mongoc_cursor_destroy(cursor);
    mongoc_database_destroy(database);
#endif
    return available;
}

void stats_updater(void *args, zctx_t *ctx, void *pipe)
{
    size_t id = (size_t)args;
    stats_updater_state_t state;
    state.updates_count = 0;
    state.controller_socket = pipe;
    state.pull_socket = zsocket_new(ctx, ZMQ_PULL);
    assert(state.pull_socket);
    int rc = zsocket_connect(state.pull_socket, "inproc://stats-updates");
    assert(rc==0);
    for (int i = 0; i<num_databases; i++) {
        state.mongo_clients[i] = mongoc_client_new(databases[i]);
        assert(state.mongo_clients[i]);
    }
    state.stats_collections = zhash_new();
    size_t ticks = 0;

    zpoller_t *poller = zpoller_new(state.controller_socket, state.pull_socket, NULL);
    assert(poller);

    while (!zctx_interrupted) {
        // printf("[D] updater[%zu]: polling\n", id);
        // -1 == block until something is readable
        void *socket = zpoller_wait(poller, -1);
        zmsg_t *msg = NULL;
        if (socket == state.controller_socket) {
            msg = zmsg_recv(state.controller_socket);
            if (state.updates_count)
                printf("[I] updater[%zu]: tick (%zu updates)\n", id, state.updates_count);
            // ping the server
            if (ticks++ % PING_INTERVAL == 0) {
                for (int i=0; i<num_databases; i++) {
                    mongo_client_ping(state.mongo_clients[i]);
                }
            }
            // refresh database information
            if (ticks % COLLECTION_REFRESH_INTERVAL == COLLECTION_REFRESH_INTERVAL - id - 1) {
                zhash_destroy(&state.stats_collections);
                state.stats_collections = zhash_new();
            }
            state.updates_count = 0;
        } else if (socket == state.pull_socket) {
            msg = zmsg_recv(state.pull_socket);
            state.updates_count++;
            int64_t start_time_ms = zclock_time();

            zframe_t *task_frame = zmsg_first(msg);
            zframe_t *db_frame = zmsg_next(msg);
            zframe_t *stream_frame = zmsg_next(msg);
            zframe_t *hash_frame = zmsg_next(msg);

            assert(zframe_size(task_frame) == 1);
            char task_type = *(char*)zframe_data(task_frame);

            size_t n = zframe_size(db_frame);
            char db_name[n+1];
            memcpy(db_name, zframe_data(db_frame), n);
            db_name[n] = '\0';

            stream_info_t *stream_info;
            assert(zframe_size(stream_frame) == sizeof(stream_info));
            memcpy(&stream_info, zframe_data(stream_frame), sizeof(stream_info));

            zhash_t *updates;
            assert(zframe_size(hash_frame) == sizeof(updates));
            memcpy(&updates, zframe_data(hash_frame), sizeof(updates));

            stats_collections_t *collections = stats_updater_get_collections(&state, db_name, stream_info);
            collection_update_callback_t cb;
            cb.db_name = db_name;

            switch (task_type) {
            case 't':
                cb.collection = collections->totals;
                zhash_foreach(updates, totals_add_increments, &cb);
                break;
            case 'm':
                cb.collection = collections->minutes;
                zhash_foreach(updates, minutes_add_increments, &cb);
                break;
            case 'q':
                cb.collection = collections->quants;
                zhash_foreach(updates, quants_add_quants, &cb);
                break;
            default:
                fprintf(stderr, "[E] updater[%zu]: unknown task type: %c\n", id, task_type);
                assert(false);
            }
            zhash_destroy(&updates);

            int64_t end_time_ms = zclock_time();
            printf("[I] updater[%zu]: task[%c]: (%3d ms) %s\n", id, task_type, (int)(end_time_ms - start_time_ms), db_name);
        } else {
            printf("[I] updater[%zu]: no socket input. interrupted = %d\n", id, zctx_interrupted);
            break;
        }
        zmsg_destroy(&msg);
    }

    zhash_destroy(&state.stats_collections);
    for (int i = 0; i<num_databases; i++) {
        mongoc_client_destroy(state.mongo_clients[i]);
    }
    printf("[I] updater[%zu]: terminated\n", id);
}

void* request_writer_pull_socket_new(zctx_t *context, int i)
{
    void *socket = zsocket_new(context, ZMQ_PULL);
    assert(socket);
    int rc = zsocket_bind(socket, "inproc://request-writer-%d", i);
    assert(rc == 0);
    return socket;
}

void add_request_field_index(const char* field, mongoc_collection_t *requests_collection)
{
    bson_error_t error;
    bson_t *index_keys;

    // collection.create_index([ [f, 1] ], :background => true, :sparse => true)
    index_keys = bson_new();
    bson_append_int32(index_keys, field, strlen(field), 1);
    if (!mongoc_collection_create_index(requests_collection, index_keys, &index_opt_sparse, &error)) {
        fprintf(stderr, "[E] index creation failed: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(index_keys);

    // collection.create_index([ ["page", 1], [f, 1] ], :background => true)
    index_keys = bson_new();
    bson_append_int32(index_keys, "page", 4, 1);
    bson_append_int32(index_keys, field, strlen(field), 1);
    if (!mongoc_collection_create_index(requests_collection, index_keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] index creation failed: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(index_keys);
}

void add_request_collection_indexes(const char* db_name, mongoc_collection_t *requests_collection)
{
    bson_error_t error;
    bson_t *index_keys;

    // collection.create_index([ ["metrics.n", 1], ["metrics.v", -1] ], :background => true)
    index_keys = bson_new();
    bson_append_int32(index_keys, "metrics.n", 9, 1);
    bson_append_int32(index_keys, "metrics.v", 9, -1);
    if (!mongoc_collection_create_index(requests_collection, index_keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] index creation failed: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(index_keys);

    // collection.create_index([ ["page", 1], ["metrics.n", 1], ["metrics.v", -1] ], :background => true
    index_keys = bson_new();
    bson_append_int32(index_keys, "page", 4, 1);
    bson_append_int32(index_keys, "metrics.n", 9, 1);
    bson_append_int32(index_keys, "metrics.v", 9, -1);
    if (!mongoc_collection_create_index(requests_collection, index_keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] index creation failed: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(index_keys);

    add_request_field_index("response_code", requests_collection);
    add_request_field_index("severity",      requests_collection);
    add_request_field_index("minute",        requests_collection);
    add_request_field_index("exceptions",    requests_collection);
}

void add_jse_collection_indexes(const char* db_name, mongoc_collection_t *jse_collection)
{
    bson_error_t error;
    bson_t *index_keys;

    // collection.create_index([ ["logjam_request_id", 1] ], :background => true)
    index_keys = bson_new();
    bson_append_int32(index_keys, "logjam_request_id", 17, 1);
    if (!mongoc_collection_create_index(jse_collection, index_keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] index creation failed: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(index_keys);

    // collection.create_index([ ["description", 1] ], :background => true
    index_keys = bson_new();
    bson_append_int32(index_keys, "description", 11, 1);
    if (!mongoc_collection_create_index(jse_collection, index_keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] index creation failed: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(index_keys);
}

mongoc_collection_t* request_writer_get_request_collection(request_writer_state_t* self, const char* db_name, stream_info_t *stream_info)
{
    if (dryrun) return NULL;
    mongoc_collection_t *collection = zhash_lookup(self->request_collections, db_name);
    if (collection == NULL) {
        // printf("[D] creating requests collection: %s\n", db_name);
        mongoc_client_t *mongo_client = self->mongo_clients[stream_info->db];
        collection = mongoc_client_get_collection(mongo_client, db_name, "requests");
        // add_request_collection_indexes(db_name, collection);
        zhash_insert(self->request_collections, db_name, collection);
        zhash_freefn(self->request_collections, db_name, (zhash_free_fn*)mongoc_collection_destroy);
    }
    return collection;
}

mongoc_collection_t* request_writer_get_jse_collection(request_writer_state_t* self, const char* db_name, stream_info_t *stream_info)
{
    if (dryrun) return NULL;
    mongoc_collection_t *collection = zhash_lookup(self->jse_collections, db_name);
    if (collection == NULL) {
        // printf("[D] creating jse collection: %s\n", db_name);
        mongoc_client_t *mongo_client = self->mongo_clients[stream_info->db];
        collection = mongoc_client_get_collection(mongo_client, db_name, "js_exceptions");
        // add_jse_collection_indexes(db_name, collection);
        zhash_insert(self->jse_collections, db_name, collection);
        zhash_freefn(self->jse_collections, db_name, (zhash_free_fn*)mongoc_collection_destroy);
    }
    return collection;
}

mongoc_collection_t* request_writer_get_events_collection(request_writer_state_t* self, const char* db_name, stream_info_t *stream_info)
{
    if (dryrun) return NULL;
    mongoc_collection_t *collection = zhash_lookup(self->events_collections, db_name);
    if (collection == NULL) {
        // printf("[D] creating events collection: %s\n", db_name);
        mongoc_client_t *mongo_client = self->mongo_clients[stream_info->db];
        collection = mongoc_client_get_collection(mongo_client, db_name, "events");
        zhash_insert(self->events_collections, db_name, collection);
        zhash_freefn(self->events_collections, db_name, (zhash_free_fn*)mongoc_collection_destroy);
    }
    return collection;
}

int bson_append_win1252(bson_t *b, const char *key, size_t key_len, const char* val, size_t val_len)
{
    char utf8[6*val_len+1];
    int new_len = convert_to_win1252(val, val_len, utf8);
    return bson_append_utf8(b, key, key_len, utf8, new_len);
}


static void json_object_to_bson(const char* context, json_object *j, bson_t *b);

//TODO: optimize this!
static void json_key_to_bson_key(const char* context, bson_t *b, json_object *val, const char *key)
{
    size_t n = strlen(key);
    char safe_key[4*n+1];
    int len = copy_replace_dots_and_dollars(safe_key, key);

    if (!bson_utf8_validate(safe_key, len, false)) {
        char tmp[6*len+1];
        len = convert_to_win1252(safe_key, len, tmp);
        strcpy(safe_key, tmp);
    }
    // printf("[D] safe_key: %s\n", safe_key);

    enum json_type type = json_object_get_type(val);
    switch (type) {
    case json_type_boolean:
        bson_append_bool(b, safe_key, len, json_object_get_boolean(val));
        break;
    case json_type_double:
        bson_append_double(b, safe_key, len, json_object_get_double(val));
        break;
    case json_type_int:
        bson_append_int32(b, safe_key, len, json_object_get_int(val));
        break;
    case json_type_object: {
        bson_t *sub = bson_new();
        json_object_to_bson(context, val, sub);
        bson_append_document(b, safe_key, len, sub);
        bson_destroy(sub);
        break;
    }
    case json_type_array: {
        bson_t *sub = bson_new();
        int array_len = json_object_array_length(val);
        for (int pos = 0; pos < array_len; pos++) {
            char nk[100];
            sprintf(nk, "%d", pos);
            json_key_to_bson_key(context, sub, json_object_array_get_idx(val, pos), nk);
        }
        bson_append_array(b, safe_key, len, sub);
        bson_destroy(sub);
        break;
    }
    case json_type_string: {
        const char *str = json_object_get_string(val);
        size_t n = json_object_get_string_len(val);
        if (bson_utf8_validate(str, n, false /* disallow embedded null characters */)) {
            bson_append_utf8(b, safe_key, len, str, n);
        } else {
            fprintf(stderr,
                    "[W] invalid utf8. context: %s,  key: %s, value[len=%d]: %*s\n",
                    context, safe_key, (int)n, (int)n, str);
            // bson_append_binary(b, safe_key, len, BSON_SUBTYPE_BINARY, (uint8_t*)str, n);
            bson_append_win1252(b, safe_key, len, str, n);
        }
        break;
    }
    case json_type_null:
        bson_append_null(b, safe_key, len);
        break;
    default:
        fprintf(stderr, "[E] unexpected json type: %s\n", json_type_to_name(type));
        break;
    }
}

static void json_object_to_bson(const char *context, json_object *j, bson_t* b)
{
  json_object_object_foreach(j, key, val) {
      json_key_to_bson_key(context, b, val, key);
  }
}

bool json_object_is_zero(json_object* jobj)
{
    enum json_type type = json_object_get_type(jobj);
    if (type == json_type_double) {
        return 0.0 == json_object_get_double(jobj);
    }
    else if (type == json_type_int) {
        return 0 == json_object_get_int(jobj);
    }
    return false;
}

void convert_metrics_for_indexing(json_object *request)
{
    json_object *metrics = json_object_new_array();
    for (int i=0; i<=last_resource_index; i++) {
        const char* resource = int_to_resource[i];
        json_object *resource_val;
        if (json_object_object_get_ex(request, resource, &resource_val)) {
            json_object_get(resource_val);
            json_object_object_del(request, resource);
            if (json_object_is_zero(resource_val)) {
                json_object_put(resource_val);
            } else {
                json_object *metric_pair = json_object_new_object();
                json_object_object_add(metric_pair, "n", json_object_new_string(resource));
                json_object_object_add(metric_pair, "v", resource_val);
                json_object_array_add(metrics, metric_pair);
            }
        }
    }
    json_object_object_add(request, "metrics", metrics);
}

json_object* store_request(const char* db_name, stream_info_t* stream_info, json_object* request, request_writer_state_t* state)
{
    // dump_json_object(stdout, request);
    convert_metrics_for_indexing(request);

    mongoc_collection_t *requests_collection = request_writer_get_request_collection(state, db_name, stream_info);
    bson_t *document = bson_sized_new(2048);

    json_object *request_id_obj;
    const char *request_id = NULL;
    if (json_object_object_get_ex(request, "request_id", &request_id_obj)) {
        request_id = json_object_get_string(request_id_obj);
        int len = strlen(request_id);
        if (len != 32) {
            // this can't be a UUID
            fprintf(stderr, "[W] not a valid uuid: %s\n", request_id);
            request_id = NULL;
            request_id_obj = NULL;
        } else {
            json_object_get(request_id_obj);
            bson_append_binary(document, "_id", 3, BSON_SUBTYPE_UUID_DEPRECATED, (uint8_t*)request_id, 32);
        }
        json_object_object_del(request, "request_id");
    }
    if (request_id == NULL) {
        // generate an oid
        bson_oid_t oid;
        bson_oid_init(&oid, NULL);
        bson_append_oid(document, "_id", 3, &oid);
        // printf("[D] generated oid for document:\n");
    }
    {
        size_t n = 1024;
        char context[n];
        snprintf(context, n, "%s:%s", db_name, request_id);
        json_object_to_bson(context, request, document);
    }

    // size_t n;
    // char* bs = bson_as_json(document, &n);
    // printf("[D] doument. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_insert(requests_collection, MONGOC_INSERT_NONE, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying request insert operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] insert failed for request document with rid '%s' on %s: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        request_id, db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(document);

    return request_id_obj;
}

void store_js_exception(const char* db_name, stream_info_t *stream_info, json_object* request, request_writer_state_t* state)
{
    mongoc_collection_t *jse_collection = request_writer_get_jse_collection(state, db_name, stream_info);
    bson_t *document = bson_sized_new(1024);
    json_object_to_bson("js_excpetion", request, document);

    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_insert(jse_collection, MONGOC_INSERT_NONE, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying exception insert operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] insert failed for exception document on %s: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(document);
}

void store_event(const char* db_name, stream_info_t *stream_info, json_object* request, request_writer_state_t* state)
{
    mongoc_collection_t *events_collection = request_writer_get_events_collection(state, db_name, stream_info);
    bson_t *document = bson_sized_new(1024);
    json_object_to_bson("event", request, document);

    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_insert(events_collection, MONGOC_INSERT_NONE, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying event insert operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] insert failed for event document on %s: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(document);
}

void publish_error_for_module(stream_info_t *stream_info, const char* module, const char* json_str, void* live_stream_socket)
{
    size_t n = stream_info->app_len + 1 + stream_info->env_len;
    // skip :: at the beginning of module
    while (*module == ':') module++;
    size_t m = strlen(module);
    char key[n + m + 3];
    sprintf(key, "%s-%s,%s", stream_info->app, stream_info->env, module);
    // TODO: change this crap in the live stream publisher
    // tolower is unsafe and not really necessary
    for (char *p = key; *p; ++p) *p = tolower(*p);

    live_stream_publish(live_stream_socket, key, json_str);
}

json_object* extract_error_description(json_object* request, int severity)
{
    json_object *error_line = NULL;
    json_object *lines;
    if (json_object_object_get_ex(request, "lines", &lines)) {
        int len = json_object_array_length(lines);
        for (int i=0; i<len; i++) {
            json_object* line = json_object_array_get_idx(lines, i);
            if (line) {
                json_object* sev_obj = json_object_array_get_idx(line, 0);
                if (sev_obj != NULL && json_object_get_int(sev_obj) >= severity) {
                    error_line = json_object_array_get_idx(line, 2);
                    break;
                }
            }
        }
    }
    const char *description;
    if (error_line) {
        description = json_object_get_string(error_line);
    } else {
        description = "------ unknown ------";
    }
    return json_object_new_string(description);
}

void request_writer_publish_error(stream_info_t* stream_info, const char* module, json_object* request,
                                  request_writer_state_t* state, json_object* request_id)
{
    if (request_id == NULL) return;

    json_object *severity_obj;
    if (json_object_object_get_ex(request, "severity", &severity_obj)) {
        int severity = json_object_get_int(severity_obj);
        if (severity > 1) {
            json_object *error_info = json_object_new_object();
            json_object_get(request_id);
            json_object_object_add(error_info, "request_id", request_id);

            json_object_get(severity_obj);
            json_object_object_add(error_info, "severity", severity_obj);

            json_object *action;
            assert( json_object_object_get_ex(request, "page", &action) );
            json_object_get(action);
            json_object_object_add(error_info, "action", action);

            json_object *rsp;
            assert( json_object_object_get_ex(request, "response_code", &rsp) );
            json_object_get(rsp);
            json_object_object_add(error_info, "response_code", rsp);

            json_object *started_at;
            assert( json_object_object_get_ex(request, "started_at", &started_at) );
            json_object_get(started_at);
            json_object_object_add(error_info, "time", started_at);

            json_object *description = extract_error_description(request, severity);
            json_object_object_add(error_info, "description", description);

            json_object *arror = json_object_new_array();
            json_object_array_add(arror, error_info);

            const char *json_str = json_object_to_json_string_ext(arror, JSON_C_TO_STRING_PLAIN);

            publish_error_for_module(stream_info, "all_pages", json_str, state->live_stream_socket);
            publish_error_for_module(stream_info, module, json_str, state->live_stream_socket);

            json_object_put(arror);
        }
    }

    json_object_put(request_id);
}

void handle_request_msg(zmsg_t* msg, request_writer_state_t* state)
{
    zframe_t *db_frame = zmsg_first(msg);
    zframe_t *type_frame = zmsg_next(msg);
    zframe_t *mod_frame = zmsg_next(msg);
    zframe_t *body_frame = zmsg_next(msg);
    zframe_t *stream_frame = zmsg_next(msg);

    size_t db_name_len = zframe_size(db_frame);
    char db_name[db_name_len+1];
    memcpy(db_name, zframe_data(db_frame), db_name_len);
    db_name[db_name_len] = '\0';
    // printf("[D] request_writer: db name: %s\n", db_name);

    stream_info_t *stream_info;
    assert(zframe_size(stream_frame) == sizeof(stream_info_t*));
    memcpy(&stream_info, zframe_data(stream_frame), sizeof(stream_info_t*));
    // printf("[D] request_writer: stream name: %s\n", stream_info->key);

    size_t mod_len = zframe_size(mod_frame);
    char module[mod_len+1];
    memcpy(module, zframe_data(mod_frame), mod_len);
    module[mod_len] = '\0';

    json_object *request, *request_id;
    assert(zframe_size(body_frame) == sizeof(json_object*));
    memcpy(&request, zframe_data(body_frame), sizeof(json_object*));
    // dump_json_object(stdout, request);

    assert(zframe_size(type_frame) == 1);
    char task_type = *((char*)zframe_data(type_frame));

    if (!dryrun) {
        switch (task_type) {
        case 'r':
            request_id = store_request(db_name, stream_info, request, state);
            request_writer_publish_error(stream_info, module, request, state, request_id);
            break;
        case 'j':
            store_js_exception(db_name, stream_info, request, state);
            break;
        case 'e':
            store_event(db_name, stream_info, request, state);
            break;
        default:
            fprintf(stderr, "[E] unknown task type for request_writer: %c\n", task_type);
        }
    }
    json_object_put(request);
}

void request_writer(void *args, zctx_t *ctx, void *pipe)
{
    request_writer_state_t state;
    state.id = (size_t)args;
    state.request_count = 0;
    state.controller_socket = pipe;
    state.pull_socket = request_writer_pull_socket_new(ctx, state.id);
    for (int i=0; i<num_databases; i++) {
        state.mongo_clients[i] = mongoc_client_new(databases[i]);
        assert(state.mongo_clients[i]);
    }
    state.request_collections = zhash_new();
    state.jse_collections = zhash_new();
    state.events_collections = zhash_new();
    state.live_stream_socket = live_stream_socket_new(ctx);
    size_t ticks = 0;

    zpoller_t *poller = zpoller_new(state.controller_socket, state.pull_socket, NULL);
    assert(poller);

    while (!zctx_interrupted) {
        // printf("[D] writer [%zu]: polling\n", state.id);
        // -1 == block until something is readable
        void *socket = zpoller_wait(poller, -1);
        zmsg_t *msg = NULL;
        if (socket == state.controller_socket) {
            // tick
            if (state.request_count)
                printf("[I] writer [%zu]: tick (%zu requests)\n", state.id, state.request_count);
            if (ticks++ % PING_INTERVAL == 0) {
                // ping mongodb to reestablish connection if it got lost
                for (int i=0; i<num_databases; i++) {
                    mongo_client_ping(state.mongo_clients[i]);
                }
            }
            // free collection pointers every hour
            msg = zmsg_recv(state.controller_socket);
            if (ticks % COLLECTION_REFRESH_INTERVAL == COLLECTION_REFRESH_INTERVAL - state.id - 1) {
                printf("[I] writer [%zu]: freeing request collections\n", state.id);
                zhash_destroy(&state.request_collections);
                zhash_destroy(&state.jse_collections);
                zhash_destroy(&state.events_collections);
                state.request_collections = zhash_new();
                state.jse_collections = zhash_new();
                state.events_collections = zhash_new();
            }
            state.request_count = 0;
        } else if (socket == state.pull_socket) {
            msg = zmsg_recv(state.pull_socket);
            if (msg != NULL) {
                state.request_count++;
                handle_request_msg(msg, &state);
            }
        } else {
            // interrupted
            printf("[I] writer [%zu]: no socket input. interrupted = %d\n", state.id, zctx_interrupted);
            break;
        }
        zmsg_destroy(&msg);
    }

    zhash_destroy(&state.request_collections);
    zhash_destroy(&state.jse_collections);
    zhash_destroy(&state.events_collections);
    for (int i=0; i<num_databases; i++) {
        mongoc_client_destroy(state.mongo_clients[i]);
    }
    printf("[I] writer [%zu]: terminated\n", state.id);
}

void *indexer_pull_socket_new(zctx_t *ctx)
{
    void *socket = zsocket_new(ctx, ZMQ_PULL);
    assert(socket);
    int rc = zsocket_bind(socket, "inproc://indexer");
    assert(rc == 0);
    return socket;
}

void indexer_create_indexes(indexer_state_t *state, const char *db_name, stream_info_t *stream_info)
{
    mongoc_client_t *client = state->mongo_clients[stream_info->db];
    mongoc_collection_t *collection;
    bson_error_t error;
    bson_t *keys;
    size_t id = state->id;

    if (dryrun) return;

    // if it is a db of today, then make it known
    if (strstr(db_name, iso_date_today)) {
        printf("[I] indexer[%zu]: ensuring known database: %s\n", id, db_name);
        ensure_known_database(client, db_name);
    }
    printf("[I] indexer[%zu]: creating indexes for %s\n", id, db_name);

    collection = mongoc_client_get_collection(client, db_name, "totals");
    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    if (!mongoc_collection_create_index(collection, keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] indexer[%zu]: index creation failed: (%d) %s\n", id, error.code, error.message);
    }
    bson_destroy(keys);
    mongoc_collection_destroy(collection);

    collection = mongoc_client_get_collection(client, db_name, "minutes");
    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    assert(bson_append_int32(keys, "minutes", 6, 1));
    if (!mongoc_collection_create_index(collection, keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] indexer[%zu]: index creation failed: (%d) %s\n", id, error.code, error.message);
    }
    bson_destroy(keys);
    mongoc_collection_destroy(collection);

    collection = mongoc_client_get_collection(client, db_name, "quants");
    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    assert(bson_append_int32(keys, "kind", 4, 1));
    assert(bson_append_int32(keys, "quant", 5, 1));
    if (!mongoc_collection_create_index(collection, keys, &index_opt_default, &error)) {
        fprintf(stderr, "[E] indexer[%zu]: index creation failed: (%d) %s\n", id, error.code, error.message);
    }
    bson_destroy(keys);
    mongoc_collection_destroy(collection);

    collection = mongoc_client_get_collection(client, db_name, "requests");
    add_request_collection_indexes(db_name, collection);
    mongoc_collection_destroy(collection);

    collection = mongoc_client_get_collection(client, db_name, "js_exceptions");
    add_jse_collection_indexes(db_name, collection);
    mongoc_collection_destroy(collection);
}

void handle_indexer_request(zmsg_t *msg, indexer_state_t *state)
{
    zframe_t *db_frame = zmsg_first(msg);
    zframe_t *stream_frame = zmsg_next(msg);

    size_t n = zframe_size(db_frame);
    char db_name[n+1];
    memcpy(db_name, zframe_data(db_frame), n);
    db_name[n] = '\0';

    stream_info_t *stream_info;
    assert(zframe_size(stream_frame) == sizeof(stream_info_t*));
    memcpy(&stream_info, zframe_data(stream_frame), sizeof(stream_info_t*));

    const char *known_db = zhash_lookup(state->databases, db_name);
    if (known_db == NULL) {
        zhash_insert(state->databases, db_name, strdup(db_name));
        zhash_freefn(state->databases, db_name, free);
        indexer_create_indexes(state, db_name, stream_info);
    } else {
        // printf("[D] indexer[%zu]: indexes already created: %s\n", state->id, db_name);
    }
}

void indexer_create_all_indexes(indexer_state_t *self, const char *iso_date, int delay)
{
    zlist_t *streams = zhash_keys(configured_streams);
    char *stream = zlist_first(streams);
    bool have_subscriptions = zhash_size(stream_subscriptions) > 0;
    while (stream && !zctx_interrupted) {
        stream_info_t *info = zhash_lookup(configured_streams, stream);
        assert(info);
        if (!have_subscriptions || zhash_lookup(stream_subscriptions, stream)) {
            char db_name[1000];
            sprintf(db_name, "logjam-%s-%s-%s", info->app, info->env, iso_date);
            indexer_create_indexes(self, db_name, info);
            if (delay) {
                zclock_sleep(1000 * delay);
            }
        }
        stream = zlist_next(streams);
    }
    zlist_destroy(&streams);
}

typedef struct {
    size_t id;
    char iso_date[ISO_DATE_STR_LEN];
} bg_indexer_args_t;

void* create_indexes_for_date(void* args)
{
    indexer_state_t state;
    memset(&state, 0, sizeof(state));
    bg_indexer_args_t *indexer_args = args;
    state.id = indexer_args->id;;

    for (int i=0; i<num_databases; i++) {
        state.mongo_clients[i] = mongoc_client_new(databases[i]);
        assert(state.mongo_clients[i]);
    }
    state.databases = zhash_new();

    indexer_create_all_indexes(&state, indexer_args->iso_date, 10);

    zhash_destroy(&state.databases);
    for (int i=0; i<num_databases; i++) {
        mongoc_client_destroy(state.mongo_clients[i]);
    }

    free(indexer_args);
    return NULL;
}

void spawn_bg_indexer_for_date(size_t id, const char* iso_date)
{
    bg_indexer_args_t *indexer_args = malloc(sizeof(bg_indexer_args_t));
    assert(indexer_args != NULL);
    indexer_args->id = id;
    strcpy(indexer_args->iso_date, iso_date);
    zthread_new(create_indexes_for_date, indexer_args);
}

void indexer(void *args, zctx_t *ctx, void *pipe)
{
    indexer_state_t state;
    memset(&state, 0, sizeof(state));
    state.controller_socket = pipe;
    state.pull_socket = indexer_pull_socket_new(ctx);
    for (int i=0; i<num_databases; i++) {
        state.mongo_clients[i] = mongoc_client_new(databases[i]);
        assert(state.mongo_clients[i]);
    }
    state.databases = zhash_new();
    size_t ticks = 0;
    size_t bg_indexer_runs = 0;
    { // setup indexes (for today and tomorrow)
        update_date_info();
        indexer_create_all_indexes(&state, iso_date_today, 0);
        zmsg_t *msg = zmsg_new();
        zmsg_addstr(msg, "started");
        zmsg_send(&msg, state.controller_socket);
        spawn_bg_indexer_for_date(++bg_indexer_runs, iso_date_tomorrow);
    }

    zpoller_t *poller = zpoller_new(state.controller_socket, state.pull_socket, NULL);
    assert(poller);

    while (!zctx_interrupted) {
        // printf("indexer[%zu]: polling\n", id);
        // -1 == block until something is readable
        void *socket = zpoller_wait(poller, -1);
        zmsg_t *msg = NULL;
        if (socket == state.controller_socket) {
            // tick
            // printf("[D] indexer[%zu]: tick\n", state.id);
            msg = zmsg_recv(state.controller_socket);

            // if date has changed, start a bg thread to create databases for the next day
            if (update_date_info()) {
                printf("[I] indexer[%zu]: date change. creating indexes for tomorrow\n", state.id);
                spawn_bg_indexer_for_date(++bg_indexer_runs, iso_date_tomorrow);
            }

            if (ticks++ % PING_INTERVAL == 0) {
                // ping mongodb to reestablish connection if it got lost
                for (int i=0; i<num_databases; i++) {
                    mongo_client_ping(state.mongo_clients[i]);
                }
            }

            // free collection pointers every hour
            if (ticks % COLLECTION_REFRESH_INTERVAL == COLLECTION_REFRESH_INTERVAL - state.id - 1) {
                printf("[I] indexer[%zu]: freeing database info\n", state.id);
                zhash_destroy(&state.databases);
                state.databases = zhash_new();
            }
        } else if (socket == state.pull_socket) {
            msg = zmsg_recv(state.pull_socket);
            if (msg != NULL) {
                handle_indexer_request(msg, &state);
            }
        } else {
            // interrupted
            printf("[I] indexer[%zu]: no socket input. interrupted = %d\n", state.id, zctx_interrupted);
            break;
        }
        zmsg_destroy(&msg);
    }

    zhash_destroy(&state.databases);
    for (int i=0; i<num_databases; i++) {
        mongoc_client_destroy(state.mongo_clients[i]);
    }
    printf("[I] indexer[%zu]: terminated\n", state.id);
}


int add_modules(const char* module, void* data, void* arg)
{
    hash_pair_t *pair = arg;
    char *dest = zhash_lookup(pair->target, module);
    if (dest == NULL) {
        zhash_insert(pair->target, module, data);
        zhash_freefn(pair->target, module, free);
        zhash_freefn(pair->source, module, NULL);
    }
    return 0;
}

int add_increments(const char* namespace, void* data, void* arg)
{
    hash_pair_t *pair = arg;
    increments_t *dest_increments = zhash_lookup(pair->target, namespace);
    if (dest_increments == NULL) {
        zhash_insert(pair->target, namespace, data);
        zhash_freefn(pair->target, namespace, increments_destroy);
        zhash_freefn(pair->source, namespace, NULL);
    } else {
        increments_add(dest_increments, (increments_t*)data);
    }
    return 0;
}

void combine_modules(zhash_t* target, zhash_t *source)
{
    hash_pair_t hash_pair;
    hash_pair.source = source;
    hash_pair.target = target;
    zhash_foreach(source, add_modules, &hash_pair);
}

void combine_increments(zhash_t* target, zhash_t *source)
{
    hash_pair_t hash_pair;
    hash_pair.source = source;
    hash_pair.target = target;
    zhash_foreach(source, add_increments, &hash_pair);
}

void combine_processors(processor_state_t* target, processor_state_t* source)
{
    // printf("[D] combining %s\n", target->db_name);
    assert(!strcmp(target->db_name, source->db_name));
    target->request_count += source->request_count;
    combine_modules(target->modules, source->modules);
    combine_increments(target->totals, source->totals);
    combine_increments(target->minutes, source->minutes);
    combine_quants(target->quants, source->quants);
}

int merge_processors(const char* db_name, void* data, void* arg)
{
    hash_pair_t *pair = arg;
    // printf("[D] checking %s\n", db_name);
    processor_state_t *dest = zhash_lookup(pair->target, db_name);
    if (dest == NULL) {
        zhash_insert(pair->target, db_name, data);
        zhash_freefn(pair->target, db_name, processor_destroy);
        zhash_freefn(pair->source, db_name, NULL);
    } else {
        combine_processors(dest, (processor_state_t*)data);
    }
    return 0;
}

int collect_stats_and_forward(zloop_t *loop, int timer_id, void *arg)
{
    int64_t start_time_ms = zclock_time();
    controller_state_t *state = arg;
    zhash_t *processors[NUM_PARSERS];
    size_t parsed_msgs_counts[NUM_PARSERS];

    state->ticks++;

    for (size_t i=0; i<NUM_PARSERS; i++) {
        void* parser_pipe = state->parser_pipes[i];
        zmsg_t *tick = zmsg_new();
        zmsg_addstr(tick, "tick");
        zmsg_send(&tick, parser_pipe);
        zmsg_t *response = zmsg_recv(parser_pipe);
        extract_parser_state(response, &processors[i], &parsed_msgs_counts[i]);
        zmsg_destroy(&response);
    }

    size_t parsed_msgs_count = parsed_msgs_counts[0];
    for (size_t i=1; i<NUM_PARSERS; i++) {
        parsed_msgs_count += parsed_msgs_counts[i];
    }

    for (size_t i=1; i<NUM_PARSERS; i++) {
        hash_pair_t pair;
        pair.source = processors[i];
        pair.target = processors[0];
        zhash_foreach(pair.source, merge_processors, &pair);
        zhash_destroy(&processors[i]);
    }

    // publish on live stream (need to do this while we still own the processors)
    zhash_foreach(processors[0], processor_publish_totals, state->live_stream_socket);

    // tell indexer to tick
    {
        zmsg_t *tick = zmsg_new();
        zmsg_addstr(tick, "tick");
        zmsg_send(&tick, state->indexer_pipe);
    }

    // tell stats updaters to tick
    for (int i=0; i<NUM_UPDATERS; i++) {
        zmsg_t *tick = zmsg_new();
        zmsg_addstr(tick, "tick");
        zmsg_send(&tick, state->updater_pipes[i]);
    }

    // forward to stats_updaters
    zlist_t *db_names = zhash_keys(processors[0]);
    const char* db_name = zlist_first(db_names);
    while (db_name != NULL) {
        processor_state_t *proc = zhash_lookup(processors[0], db_name);
        // printf("[D] forwarding %s\n", db_name);
        zmsg_t *stats_msg;

        // send totals updates
        stats_msg = zmsg_new();
        zmsg_addstr(stats_msg, "t");
        zmsg_addstr(stats_msg, proc->db_name);
        zmsg_addmem(stats_msg, &proc->stream_info, sizeof(proc->stream_info));
        zmsg_addmem(stats_msg, &proc->totals, sizeof(proc->totals));
        proc->totals = NULL;
        if (!output_socket_ready(state->updates_socket, 0)) {
            fprintf(stderr, "[W] controller: updates push socket not ready\n");
        }
        zmsg_send(&stats_msg, state->updates_socket);

        // send minutes updates
        stats_msg = zmsg_new();
        zmsg_addstr(stats_msg, "m");
        zmsg_addstr(stats_msg, proc->db_name);
        zmsg_addmem(stats_msg, &proc->stream_info, sizeof(proc->stream_info));
        zmsg_addmem(stats_msg, &proc->minutes, sizeof(proc->minutes));
        proc->minutes = NULL;
        if (!output_socket_ready(state->updates_socket, 0)) {
            fprintf(stderr, "[W] controller: updates push socket not ready\n");
        }
        zmsg_send(&stats_msg, state->updates_socket);

        // send quants updates
        stats_msg = zmsg_new();
        zmsg_addstr(stats_msg, "q");
        zmsg_addstr(stats_msg, proc->db_name);
        zmsg_addmem(stats_msg, &proc->stream_info, sizeof(proc->stream_info));
        zmsg_addmem(stats_msg, &proc->quants, sizeof(proc->quants));
        proc->quants = NULL;
        if (!output_socket_ready(state->updates_socket, 0)) {
            fprintf(stderr, "[W] controller: updates push socket not ready\n");
        }
        zmsg_send(&stats_msg, state->updates_socket);

        db_name = zlist_next(db_names);
    }
    zlist_destroy(&db_names);
    zhash_destroy(&processors[0]);

    // tell request writers to tick
    for (int i=0; i<NUM_WRITERS; i++) {
        zmsg_t *tick = zmsg_new();
        zmsg_addstr(tick, "tick");
        zmsg_send(&tick, state->writer_pipes[i]);
    }

    bool terminate = (state->ticks % CONFIG_FILE_CHECK_INTERVAL == 0) && config_file_has_changed();
    int64_t end_time_ms = zclock_time();
    int runtime = end_time_ms - start_time_ms;
    int next_tick = runtime > 999 ? 1 : 1000 - runtime;
    printf("[I] controller: %5zu messages (%d ms)\n", parsed_msgs_count, runtime);

    if (terminate) {
        printf("[I] controller: detected config change. terminating.\n");
        zctx_interrupted = 1;
    } else {
        int rc = zloop_timer(loop, next_tick, 1, collect_stats_and_forward, state);
        assert(rc != -1);
    }

    return 0;
}

void add_resources_of_type(const char *type, char **type_map, size_t *type_idx)
{
    char path[256] = {'\0'};
    strcpy(path, "metrics/");
    strcpy(path+strlen("metrics/"), type);
    zconfig_t *metrics = zconfig_locate(config, path);
    assert(metrics);
    zconfig_t *metric = zconfig_child(metrics);
    assert(metric);
    do {
        char *resource = zconfig_name(metric);
        zhash_insert(resource_to_int, resource, (void*)last_resource_index);
        int_to_resource[last_resource_index] = resource;
        char resource_sq[256] = {'\0'};
        strcpy(resource_sq, resource);
        strcpy(resource_sq+strlen(resource), "_sq");
        int_to_resource_sq[last_resource_index++] = strdup(resource_sq);
        type_map[(*type_idx)++] = resource;
        metric = zconfig_next(metric);
        assert(last_resource_index < MAX_RESOURCE_COUNT);
    } while (metric);
    (*type_idx) -= 1;

    // set up other_time_resources
    if (!strcmp(type, "time")) {
        for (size_t k = 0; k <= *type_idx; k++) {
            char *r = type_map[k];
            if (strcmp(r, "total_time") && strcmp(r, "gc_time") && strcmp(r, "other_time")) {
                other_time_resources[last_other_time_resource_index++] = r;
            }
        }
        last_other_time_resource_index--;

        // printf("[D] other time resources:\n");
        // for (size_t j=0; j<=last_other_time_resource_index; j++) {
        //      printf("[D] %s\n", other_time_resources[j]);
        // }
    }

    // printf("[D] %s resources:\n", type);
    // for (size_t j=0; j<=*type_idx; j++) {
    //     printf("[D] %s\n", type_map[j]);
    // }
}

// setup bidirectional mapping between resource names and small integers
void setup_resource_maps()
{
    //TODO: move this to autoconf
    assert(sizeof(size_t) == sizeof(void*));

    resource_to_int = zhash_new();
    add_resources_of_type("time", time_resources, &last_time_resource_index);
    add_resources_of_type("call", call_resources, &last_call_resource_index);
    add_resources_of_type("memory", memory_resources, &last_memory_resource_index);
    add_resources_of_type("heap", heap_resources, &last_heap_resource_index);
    add_resources_of_type("frontend", frontend_resources, &last_frontend_resource_index);
    add_resources_of_type("dom", dom_resources, &last_dom_resource_index);
    last_resource_index--;

    allocated_objects_index = r2i("allocated_memory");
    allocated_bytes_index = r2i("allocated_bytes");

    // for (size_t j=0; j<=last_resource_index; j++) {
    //     const char *r = i2r(j);
    //     printf("[D] %s = %zu\n", r, r2i(r));
    // }
}


zlist_t* get_stream_settings(stream_info_t *info, const char* name)
{
    zconfig_t *setting;
    char key[528] = {'0'};

    zlist_t *settings = zlist_new();
    sprintf(key, "backend/streams/%s/%s", info->key, name);
    setting = zconfig_locate(config, key);
    if (setting)
        zlist_push(settings, setting);

    sprintf(key, "backend/defaults/environments/%s/%s", info->env, name);
    setting = zconfig_locate(config, key);
    if (setting)
        zlist_push(settings, setting);

    sprintf(key, "backend/defaults/applications/%s/%s", info->app, name);
    setting = zconfig_locate(config, key);
    if (setting)
        zlist_push(settings, setting);

    sprintf(key, "backend/defaults/%s", name);
    setting = zconfig_locate(config, key);
    if (setting)
        zlist_push(settings, setting);

    return settings;
}

void add_threshold_settings(stream_info_t* info)
{
    info->import_threshold = global_total_time_import_threshold;
    zlist_t *settings = get_stream_settings(info, "import_threshold");
    zconfig_t *setting = zlist_first(settings);
    zhash_t *module_settings = zhash_new();
    while (setting) {
        info->import_threshold = atoi(zconfig_value(setting));
        zconfig_t *module_setting = zconfig_child(setting);
        while (module_setting) {
            char *module_name = zconfig_name(module_setting);
            size_t threshold_value = atoi(zconfig_value(module_setting));
            zhash_update(module_settings, module_name, (void*)threshold_value);
            module_setting = zconfig_next(module_setting);
        }
        setting = zlist_next(settings);
    }
    zlist_destroy(&settings);
    int n = zhash_size(module_settings);
    info->module_threshold_count = n;
    info->module_thresholds = malloc(n * sizeof(module_threshold_t));
    zlist_t *modules = zhash_keys(module_settings);
    int i = 0;
    const char *module = zlist_first(modules);
    while (module) {
        info->module_thresholds[i].name = strdup(module);
        info->module_thresholds[i].value = (size_t)zhash_lookup(module_settings, module);
        i++;
        module = zlist_next(modules);
    }
    zlist_destroy(&modules);
    zhash_destroy(&module_settings);
}

void add_ignored_request_settings(stream_info_t* info)
{
    info->ignored_request_prefix = global_ignored_request_prefix;
    zlist_t* settings = get_stream_settings(info, "ignored_request_uri");
    zconfig_t *setting = zlist_first(settings);
    while (setting) {
        info->ignored_request_prefix = zconfig_value(setting);
        setting = zlist_next(settings);
    }
    zlist_destroy(&settings);
}

stream_info_t* stream_info_new(zconfig_t *stream_config)
{
    stream_info_t *info = malloc(sizeof(stream_info_t));
    info->key = zconfig_name(stream_config);
    if (!strncmp(info->key, "request-stream-", 15)) {
        info->key += 15;
    }
    info->key_len = strlen(info->key);

    char app[256] = {'\0'};
    char env[256] = {'\0'};;
    int n = sscanf(info->key, "%[^-]-%[^-]", app, env);
    assert(n == 2);

    info->app = strdup(app);
    info->app_len = strlen(app);
    assert(info->app_len > 0);

    info->env = strdup(env);
    info->env_len = strlen(env);
    assert(info->env_len > 0);

    info->db = 0;
    zconfig_t *db_setting = zconfig_locate(stream_config, "db");
    if (db_setting) {
        const char* dbval = zconfig_value(db_setting);
        int db_num = atoi(dbval);
        // printf("[D] db for %s-%s: %d (numdbs: %zu)\n", info->app, info->env, db_num, num_databases);
        assert(db_num < num_databases);
        info->db = db_num;
    }
    add_threshold_settings(info);
    add_ignored_request_settings(info);

    return info;
}

void dump_stream_info(stream_info_t *stream)
{
    printf("[D] key: %s\n", stream->key);
    printf("[D] app: %s\n", stream->app);
    printf("[D] env: %s\n", stream->env);
    printf("[D] ignored_request_uri: %s\n", stream->ignored_request_prefix);
    printf("[D] import_threshold: %d\n", stream->import_threshold);
    for (int i = 0; i<stream->module_threshold_count; i++) {
        printf("[D] module_threshold: %s = %zu\n", stream->module_thresholds[i].name, stream->module_thresholds[i].value);
    }
}

void setup_stream_config()
{
    bool have_subscription_pattern = strcmp("", subscription_pattern);

    zconfig_t *import_threshold_config = zconfig_locate(config, "backend/defaults/import_threshold");
    if (import_threshold_config) {
        int t = atoi(zconfig_value(import_threshold_config));
        // printf("[D] setting global import threshold: %d\n", t);
        global_total_time_import_threshold = t;
    }

    zconfig_t *ignored_requests_config = zconfig_locate(config, "backend/defaults/ignored_request_uri");
    if (ignored_requests_config) {
        const char *prefix = zconfig_value(ignored_requests_config);
        // printf("[D] setting global ignored_requests uri: %s\n", prefix);
        global_ignored_request_prefix = prefix;
    }

    configured_streams = zhash_new();
    stream_subscriptions = zhash_new();

    zconfig_t *all_streams = zconfig_locate(config, "backend/streams");
    assert(all_streams);
    zconfig_t *stream = zconfig_child(all_streams);
    assert(stream);

    do {
        stream_info_t *stream_info = stream_info_new(stream);
        const char *key = stream_info->key;
        // dump_stream_info(stream_info);
        zhash_insert(configured_streams, key, stream_info);
        if (have_subscription_pattern && strstr(key, subscription_pattern) != NULL) {
            int rc = zhash_insert(stream_subscriptions, key, stream_info);
            assert(rc == 0);
        }
        stream = zconfig_next(stream);
    } while (stream);
}

void print_usage(char * const *argv)
{
    fprintf(stderr, "usage: %s [-n] [-p stream-pattern] [-c config-file]\n", argv[0]);
}

void process_arguments(int argc, char * const *argv)
{
    char c;
    opterr = 0;
    while ((c = getopt(argc, argv, "nc:p:")) != -1) {
        switch (c) {
        case 'n':
            dryrun = true;;
            break;
        case 'c':
            config_file_name = optarg;
            break;
        case 'p':
            subscription_pattern = optarg;
            break;
        case '?':
            if (optopt == 'c')
                fprintf(stderr, "[E] option -%c requires an argument.\n", optopt);
            else if (isprint (optopt))
                fprintf(stderr, "[E] unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "[E] unknown option character `\\x%x'.\n", optopt);
            print_usage(argv);
            exit(1);
        default:
            exit(1);
        }
    }
}

int main(int argc, char * const *argv)
{
    int rc;
    process_arguments(argc, argv);

    if (!zsys_file_exists(config_file_name)) {
        fprintf(stderr, "[E] missing config file: %s\n", config_file_name);
        exit(1);
    } else {
        config_file_init();
    }

    update_date_info();

    // load config
    config = zconfig_load((char*)config_file_name);
    // zconfig_print(config);
    initialize_mongo_db_globals();
    setup_resource_maps();
    setup_stream_config();

    setvbuf(stdout,NULL,_IOLBF,0);
    setvbuf(stderr,NULL,_IOLBF,0);

    // establish global zeromq context
    zctx_t *context = zctx_new();
    assert(context);
    zctx_set_rcvhwm(context, 1000);
    zctx_set_sndhwm(context, 1000);
    zctx_set_linger(context, 100);

    // set up event loop
    zloop_t *loop = zloop_new();
    assert(loop);
    zloop_set_verbose(loop, 0);

    controller_state_t state;
    state.ticks = 0;

    // start the indexer
    state.indexer_pipe = zthread_fork(context, indexer, NULL);
    {
        // wait for initial db index creation
        zmsg_t * msg = zmsg_recv(state.indexer_pipe);
        zmsg_destroy(&msg);

        if (zctx_interrupted) goto exit;
    }

    // create socket for stats updates
    state.updates_socket = zsocket_new(context, ZMQ_PUSH);
    rc = zsocket_bind(state.updates_socket, "inproc://stats-updates");
    assert(rc == 0);

    // connect to live stream
    state.live_stream_socket = live_stream_socket_new(context);

    // start all worker threads
    state.subscriber_pipe = zthread_fork(context, subscriber, &config);
    for (size_t i=0; i<NUM_WRITERS; i++) {
        state.writer_pipes[i] = zthread_fork(context, request_writer, (void*)i);
    }
    for (size_t i=0; i<NUM_UPDATERS; i++) {
        state.updater_pipes[i] = zthread_fork(context, stats_updater, (void*)i);
    }
    for (size_t i=0; i<NUM_PARSERS; i++) {
        state.parser_pipes[i] = zthread_fork(context, parser, (void*)i);
    }

    // flush increments to database every 1000 ms
    rc = zloop_timer(loop, 1000, 1, collect_stats_and_forward, &state);
    assert(rc != -1);

    if (!zctx_interrupted) {
        // run the loop
        rc = zloop_start(loop);
        printf("[I] shutting down: %d\n", rc);
    }

    // shutdown
    zloop_destroy(&loop);
    assert(loop == NULL);

 exit:
    zctx_destroy(&context);

    return 0;
}
