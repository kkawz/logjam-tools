#include "graylog-forwarder-common.h"
#include "graylog-forwarder-writer.h"
#include "gelf-message.h"

typedef struct {
    zsock_t *pipe;          // actor commands
    zsock_t *pull_socket;   // incoming messages from parsers
    zsock_t *push_socket;   // outgoing GELF messages to graylog; the GELF ZeroMQ PULL device should connect to this (not bind)
    size_t message_count;   // how many messages we have sent since last tick
} writer_state_t;

static void send_graylog_message(zmsg_t* msg, writer_state_t* state)
{
    compressed_gelf_t *compressed_gelf = zmsg_popptr(msg);
    assert(compressed_gelf);

    if (dryrun) {
        compressed_gelf_destroy(&compressed_gelf);
        return;
    }

    zmsg_t *out_msg = zmsg_new();
    assert(out_msg);
    int rc = zmsg_addmem(out_msg, compressed_gelf->data, compressed_gelf->len);
    assert(rc == 0);

    compressed_gelf_destroy(&compressed_gelf);

    if (!output_socket_ready(state->push_socket, 0)) {
        fprintf(stderr, "[W] graylog-forwarder-writer: push socket not ready. blocking!\n");
    }

    zmsg_send(&out_msg, state->push_socket);
    state->message_count++;
}

static
zsock_t* writer_pull_socket_new()
{
    zsock_t *socket = zsock_new(ZMQ_PULL);
    assert(socket);
    int rc = zsock_bind(socket, "inproc://graylog-forwarder-writer");
    assert(rc == 0);
    return socket;
}

static
zsock_t* writer_push_socket_new(zconfig_t* config)
{
    zsock_t *socket = zsock_new(ZMQ_PUSH);
    assert(socket);

    char* graylog_endpoint = zconfig_resolve(config, "/graylog/endpoint", NULL);
    if (graylog_endpoint == NULL) {
        fprintf(stderr, "[E] graylog-forwarder-writer: missing graylog endpoint configuration.\n");
        exit(1);
    }

    // bind socket, taking thread startup time into account
    // TODO: this is a hack. better let controller coordinate this
    for (int i=0; i<10; i++) {
        int rc = zsock_bind(socket, "%s", graylog_endpoint);
        if (rc != -1) {
            printf("[I] graylog-forwarder-writer: binding PUSH socket for graylog to %s\n", graylog_endpoint);
            break;
        }
        zclock_sleep(100);
    }

    // set outbound high-water-mark
    int high_water_mark = atoi(zconfig_resolve(config, "/graylog/high_water_mark", "10000"));
    printf("[I] graylog-forwarder-writer: setting high-water-mark for outbound messages to %d\n", high_water_mark);
    zsock_set_sndhwm(socket, high_water_mark);

    return socket;
}

static
writer_state_t* writer_state_new(zsock_t *pipe, zconfig_t* config)
{
    writer_state_t *state = zmalloc(sizeof(writer_state_t));
    state->pipe = pipe;
    state->pull_socket = writer_pull_socket_new();
    state->push_socket = writer_push_socket_new(config);
    return state;
}

static
void writer_state_destroy(writer_state_t **state_p)
{
    writer_state_t *state = *state_p;
    // must not destroy the pipe, as it's owned by the actor
    zsock_destroy(&state->pull_socket);
    zsock_destroy(&state->push_socket);
    free(state);
    *state_p = NULL;
}

void graylog_forwarder_writer(zsock_t *pipe, void *args)
{
    set_thread_name("graylog-forwarder-writer[0]");

    zconfig_t* config = args;
    writer_state_t *state = writer_state_new(pipe, config);
    // signal readyiness after sockets have been created
    zsock_signal(pipe, 0);

    zpoller_t *poller = zpoller_new(state->pipe, state->pull_socket, NULL);
    assert(poller);

    while (!zsys_interrupted) {
        // printf("[D] writer [%zu]: polling\n", id);
        // -1 == block until something is readable
        void *socket = zpoller_wait(poller, -1);
        zmsg_t *msg = NULL;
        if (socket == state->pipe) {
            msg = zmsg_recv(state->pipe);
            char *cmd = zmsg_popstr(msg);
            zmsg_destroy(&msg);
            if (streq(cmd, "$TERM")) {
                fprintf(stderr, "[D] graylog-forwarder-writer: received $TERM command\n");
                free(cmd);
                break;
            }
            else if (streq(cmd, "tick")) {
                printf("[I] graylog-forwarder-writer: sent %zu messages\n",
                       state->message_count);
                state->message_count = 0;
            } else {
                fprintf(stderr, "[E] graylog-forwarder-writer: received unknown command: %s\n", cmd);
                assert(false);
            }
        } else if (socket == state->pull_socket) {
            msg = zmsg_recv(state->pull_socket);
            if (msg != NULL) {
                send_graylog_message(msg, state);
                zmsg_destroy(&msg);
            }
        } else {
            // msg == NULL, probably interrupted by signal handler
            break;
        }
    }

    fprintf(stdout, "[I] graylog-forwarder-writer: shutting down\n");
    writer_state_destroy(&state);
    fprintf(stdout, "[I] graylog-forwarder-writer: terminated\n");
}