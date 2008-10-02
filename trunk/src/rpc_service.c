interface foo
{
	method bar
	{
		request
		{
			uint32 a
			int64 b
			blob c
		}
		response
		{
			int32 d
		}
	}

	method baz
	{
		request
		{
		}
		response
		{
		}
	}
}

INTERFACE ::= "interface" id "{" METHODS_LIST "}"
METHODS_LIST ::= METHOD { METHOD }
METHOD ::= "method" id "{" REQUEST_PARAMS RESPONSE_PARAMS "}"
REQUEST_PARAMS ::= "request" "{" PARAMS_LIST "}"
RESPONSE_PARAMS ::= "response" "{" PARAMS_LIST "}"
PARAMS_LIST ::= PARAM { PARAM }
PARAM ::= TYPE id
TYPE ::= "uint32" | "uint64" | "int32" | "int64" | "string" | "blob"

id = [a-z_][a-z_\d]*

#define MAX_PACKET_SIZE (1 << 12)

enum rpc_packet_type
{
	RPC_PACKET_START,
	RPC_PACKET_MIDDLE,
	RPC_PACKET_END,
	RPC_PACKET_SINGLE
};

struct rpc_packet
{
	char *buf;
	int curr_pos;
	int size;
	enum rpc_packet_type type;
	uint8_t request_id;
};

struct rpc_packet *rpc_packet_create()
{
	struct rpc_packet *packet;

	packet = (struct rpc_packet *) ff_malloc(sizeof(*packet));
	packet->buf = (char *) ff_malloc(MAX_PACKET_SIZE);
	packet->curr_pos = 0;
	packet->size = 0;
	packet->type = RPC_PACKET_START;
	packet->request_id = 0;

	return packet;
}

void rpc_packet_delete(struct rpc_packet *packet)
{
	ff_assert(packet->curr_pos == 0);
	ff_assert(packet->size == 0);

	ff_free(packet->buf);
	ff_free(packet);
}

void rpc_packet_reset(struct rpc_packet *packet)
{
	packet->curr_pos = 0;
	packet->size = 0;
}

uint8_t rpc_packet_get_request_id(struct rpc_packet *packet)
{
	return packet->request_id;
}

void rpc_packet_set_request_id(struct rpc_packet *packet, uint8_t request_id)
{
	packet->request_id = request_id;
}

enum rpc_packet_type rpc_packet_get_type(struct rpc_packet *packet)
{
	return packet->type;
}

void rpc_packet_set_type(struct rpc_packet *packet, enum rpc_packet_type type)
{
	packet->type = type;
}

int rpc_packet_read_data(struct rpc_packet *packet, void *buf, int len)
{
	int bytes_read;
	int bytes_left;

	ff_assert(len >= 0);
	ff_assert(packet->curr_pos >= 0);
	ff_assert(packet->size >= packet->curr_pos);
	ff_assert(packet->size <= MAX_PACKET_SIZE);

	bytes_left = packet->size - packet->curr_pos;
	bytes_read = (len > bytes_left) ? bytes_left : len;
	memcpy(buf, packet->buf + packet->curr_pos, bytes_read);

	return bytes_read;
}

int rpc_packet_write_data(struct rpc_packet *packet, const void *buf, int len)
{
	int bytes_written;
	int bytes_left;

	ff_assert(len >= 0);
	ff_assert(packet->curr_pos >= 0);
	ff_assert(packet->size >= packet->curr_pos);
	ff_assert(packet->size <= MAX_PACKET_SIZE);

	bytes_left = MAX_PACKET_SIZE - packet->size;
	bytes_written = (len > bytes_left) ? bytes_left : len;
	memcpy(packet->buf + packet->size, buf, bytes_written);

	return bytes_written;
}

int rpc_packet_read_from_stream(struct rpc_packet *packet, struct rpc_stream *stream)
{
	int is_success = 0;
	int bytes_read;
	uint32_t tmp;

	ff_assert(packet->curr_pos == 0);
	ff_assert(packet->size == 0);

	bytes_read = rpc_stream_read(stream, &packet->request_id, 1);
	if (bytes_read != 1)
	{
		goto end;
	}
	bytes_read = rpc_uint32_unserialize(&tmp, stream);
	if (bytes_read == -1)
	{
		goto end;
	}
	packet->type = (enum rpc_packet_type) (tmp & 0x03);
	if (packet->type != RPC_PACKET_START && packet->type != RPC_PACKET_MIDDLE && packet->type != RPC_PACKET_END && packet->type != RPC_PACKET_SINGLE)
	{
		goto end;
	}
	packet->size = (int) (tmp >> 2);
	if (packet->size > MAX_PACKET_SIZE)
	{
		goto end;
	}
	bytes_read = rpc_stream_read(stream, packet->buf, packet->size);
	if (bytes_read != packet->size)
	{
		goto end;
	}
	is_success = 1;

end:
	return is_success;
}

int rpc_packet_write_to_stream(struct rpc_packet *packet, struct rpc_stream *stream)
{
	int is_success = 0;
	int bytes_written;
	uint32_t tmp;

	ff_assert(packet->size <= MAX_PACKET_SIZE);

	bytes_written = rpc_stream_write(stream, &packet->request_id, 1);
	if (bytes_written != 1)
	{
		goto end;
	}
	tmp = ((uint32_t) packet->type) | (((uint32_t) packet->size) << 2);
	bytes_written = rpc_uint32_serialize(tmp, stream);
	if (bytes_written == -1)
	{
		goto end;
	}
	bytes_written = rpc_stream_write(stream, packet->buf, packet->size);
	if (bytes_written != packet->size)
	{
		goto end;
	}
	is_success = 1;

end:
	return is_success;
}

#define MAX_READER_QUEUE_SIZE 100
#define READ_TIMEOUT 2000
#define WRITE_TIMEOUT 2000

typedef struct rpc_packet *(*rpc_packet_stream_acquire_packet_func)(void *packet_func_ctx);
typedef void (*rpc_packet_stream_release_packet_func)(void *packet_func_ctx, struct rpc_packet *packet);

struct rpc_packet_stream
{
	rpc_packet_stream_acquire_packet_func acquire_packet_func;
	rpc_packet_stream_release_packet_func release_packet_func;
	void *packet_func_ctx;
	struct ff_blocking_queue *reader_queue;
	struct ff_blocking_queue *writer_queue;
	struct rpc_packet *current_read_packet;
	struct rpc_packet *current_write_packet;
	uint8_t request_id;
};

static struct rpc_packet *acquire_packet(struct rpc_packet_stream *stream, enum rpc_packet_type packet_type)
{
	struct rpc_packet *packet;

	ff_assert(packet_type == RPC_PACKET_START || packet_type == RPC_PACKET_MIDDLE);
	packet = stream->acquire_packet_func(stream->packet_func_ctx);
	rpc_packet_set_request_id(packet, stream->request_id);
	rpc_packet_set_type(packet, packet_type);

	return packet;
}

static void release_packet(struct rpc_packet_stream *stream, struct rpc_packet *packet)
{
	stream->release_packet_func(stream->packet_func_ctx, packet);
}

static int prefetch_current_read_packet(struct rpc_packet_stream *stream)
{
	int is_success;

	ff_assert(stream->current_read_packet == NULL);
	is_success = ff_blocking_queue_get_with_timeout(stream->reader_queue, &stream->current_read_packet, READ_TIMEOUT);
	if (is_success)
	{
		enum rpc_packet_type packet_type;

		ff_assert(stream->current_read_packet != NULL);
		packet_type = rpc_packet_get_type(stream->current_read_packet);
		if (packet_type != RPC_PACKET_START && packet_type != RPC_PACKET_SINGLE)
		{
			release_packet(stream, stream->current_read_packet);
			stream->current_read_packet = NULL;
			is_success = 0;
		}
	}
	else
	{
		ff_assert(stream->current_read_packet == NULL);
	}

	return is_success;
}

static void release_current_read_packet(struct rpc_packet_stream *stream)
{
	ff_assert(stream->current_read_packet != NULL);
	release_packet(stream, stream->current_read_packet);
	stream->current_read_packet = NULL;
}

static void acquire_current_write_packet(struct rpc_packet_stream *stream)
{
	ff_assert(stream->current_write_packet == NULL);
	stream->current_write_packet = acquire_packet(stream, RPC_PACKET_START);
}

static void release_current_write_packet(struct rpc_packet_stream *stream)
{
	enum rpc_packet_type packet_type;

	ff_assert(stream->current_write_packet != NULL);
	packet_type = rpc_packet_get_type(stream->current_write_packet);
	ff_assert(packet_type == RPC_PACKET_END);
	release_packet(stream, stream->current_write_packet);
	stream->current_write_packet = NULL;
}

struct rpc_packet_stream *rpc_packet_stream_create(struct ff_blocking_queue *writer_queue,
	rpc_packet_stream_acquire_packet_func acquire_packet_func, rpc_packet_stream_release_packet_func release_packet_func, void *packet_func_ctx)
{
	struct rpc_packet_stream *stream;

	stream = (struct rpc_packet_stream *) ff_malloc(sizeof(*stream));
	stream->acquire_packet_func = acquire_packet_func;
	stream->release_packet_func = release_packet_func;
	stream->packet_func_ctx = packet_func_ctx;
	stream->reader_queue = ff_blocking_queue_create(MAX_READER_QUEUE_SIZE);
	stream->writer_queue = writer_queue;
	stream->current_read_packet = NULL;
	stream->current_write_packet = NULL;
	stream->request_id = 0;

	return stream;
}

void rpc_packet_stream_delete(struct rpc_packet_stream *stream)
{
	ff_assert(stream->current_read_packet == NULL);
	ff_assert(stream->current_write_packet == NULL);
	ff_assert(stream->request_id == 0);

	ff_blocking_queue_delete(stream->reader_queue);
	ff_free(stream);
}

int rpc_packet_stream_initialize(struct rpc_packet_stream *stream, uint8_t request_id)
{
	int is_success;

	ff_assert(stream->current_read_packet == NULL);
	ff_assert(stream->current_write_packet == NULL);
	ff_assert(stream->request_id == 0);

	stream->request_id = request_id;
	is_success = prefetch_current_read_packet(stream);
	if (is_success)
	{
		ff_assert(stream->current_read_packet != NULL);
		acquire_current_write_packet(stream);
		ff_assert(stream->current_write_packet != NULL);
	}

	return is_success;
}

int rpc_packet_stream_shutdown(struct rpc_packet_stream *stream)
{
	int is_success;

	ff_assert(stream->current_read_packet != NULL);
	ff_assert(stream->current_write_packet != NULL);

	stream->request_id = 0;
	is_success = rpc_packet_stream_flush(stream);
	release_current_write_packet(stream);
	ff_assert(stream->current_write_packet == NULL);
	release_current_read_packet(stream);
	ff_assert(stream->current_read_packet == NULL);

	return is_success;
}

void rpc_packet_stream_clear_reader_queue(struct rpc_packet_stream *stream)
{
	for (;;)
	{
		struct rpc_packet *packet;
		int is_empty;

		is_empty = ff_blocking_queue_is_empty(stream->reader_queue);
		if (is_empty)
		{
			break;
		}
		ff_blocking_queue_get(stream->reader_queue, &packet);
		release_packet(stream, packet);
	}
}

int rpc_packet_stream_read(struct rpc_packet_stream *stream, void *buf, int len)
{
	char *p;
	enum rpc_packet_type packet_type;
	int total_bytes_read = -1;
	int bytes_to_read;

	ff_assert(len >= 0);
	ff_assert(stream->current_read_packet != NULL);

	p = (char *) buf;
	bytes_to_read = len;
	packet_type = rpc_packet_get_type(stream->current_read_packet);
	while (bytes_to_read > 0)
	{
		int bytes_read;

		bytes_read = rpc_packet_read_data(stream->current_read_packet, p, bytes_to_read);
		ff_assert(bytes_read >= 0);
		ff_assert(bytes_read <= bytes_to_read);
		bytes_to_read -= bytes_read;
		p += bytes_read;

        if (bytes_to_read > 0)
		{
			struct rpc_packet *packet;
			int is_success;

			if (packet_type == RPC_PACKET_SINGLE || packet_type == RPC_PACKET_END)
			{
				goto end;
			}

			is_success = ff_blocking_queue_get_with_timeout(stream->reader_queue, &packet, READ_TIMEOUT);
			if (!is_success)
			{
				goto end;
			}
			ff_assert(packet != NULL);
			packet_type = rpc_packet_get_type(packet);
			if (packet_type == RPC_PACKET_START || packet_type == RPC_PACKET_SINGLE)
			{
				release_packet(stream, packet);
				goto end;
			}

			release_packet(stream, stream->current_read_packet);
			stream->current_read_packet = packet;
		}
	}
	total_bytes_read = len;

end:
	return total_bytes_read;
}

int rpc_packet_stream_write(struct rpc_packet_stream *stream, const void *buf, int len)
{
	const char *p;
	enum rpc_packet_type packet_type;
	int total_bytes_written = -1;
	int bytes_to_write;

	ff_assert(len >= 0);
	ff_assert(stream->current_write_packet != NULL);

	p = (const char *) buf;
	bytes_to_write = len;
	packet_type = rpc_packet_get_type(stream->current_write_packet);
	if (packet_type == RPC_PACKET_END)
	{
		goto end;
	}
	ff_assert(packet_type == RPC_PACKET_START || packet_type == RPC_PACKET_MIDDLE);
	while (bytes_to_write > 0)
	{
		int bytes_written;

		bytes_written = rpc_packet_write_data(stream->current_write_packet, p, bytes_to_write);
		ff_assert(bytes_written >= 0);
		ff_assert(bytes_written <= bytes_to_write);
		bytes_to_write -= bytes_written;
		p += bytes_written;

		if (bytes_to_write > 0)
		{
			int is_success;

			is_success = ff_blocking_queue_put_with_timeout(stream->writer_queue, stream->current_write_packet, WRITE_TIMEOUT);
			if (!is_success)
			{
				goto end;
			}
			stream->current_write_packet = acquire_packet(stream, RPC_PACKET_MIDDLE);
		}
	}
	total_bytes_written = len;

end:
	return total_bytes_written;
}

int rpc_packet_stream_flush(struct rpc_packet_stream *stream)
{
	int is_success = 1;
	enum rpc_packet_type packet_type;

	ff_assert(stream->current_write_packet != NULL);
	packet_type = rpc_packet_get_type(stream->current_write_packet);
	if (packet_type != RPC_PACKET_END)
	{
		ff_assert(packet_type == RPC_PACKET_START || packet_type == RPC_PACKET_MIDDLE);
		if (packet_type == RPC_PACKET_START)
		{
			packet_type = RPC_PACKET_SINGLE;
		}
		else
		{
			packet_type = RPC_PACKET_END;
		}
		rpc_packet_set_type(stream->current_write_packet, packet_type);
		is_success = ff_blocking_queue_put_with_timeout(stream->writer_queue, stream->current_write_packet, WRITE_TIMEOUT);
		if (!is_success)
		{
			release_packet(stream, stream->current_write_packet);
		}
		stream->current_write_packet = acqiure_packet(stream, RPC_PACKET_END);
	}

	return is_success;
}

void rpc_packet_stream_disconnect(struct rpc_packet_stream *stream)
{
	struct rpc_packet *packet;

	rpc_packet_stream_flush(stream);
	packet = acquire_packet(stream, RPC_PACKET_END);
	ff_blocking_queue_put(stream->reader_queue, packet);
}

void rpc_packet_stream_push_packet(struct rpc_packet_stream *stream)
{
	ff_blocking_queue_put(stream->reader_queue, packet);
}

static int read_from_packet_stream(struct rpc_stream *stream, void *buf, int len)
{
	struct rpc_packet_stream *packet_stream;
	int bytes_read;

	packet_stream = (struct rpc_packet_stream *) stream->ctx;
	bytes_read = rpc_packet_stream_read(packet_stream, buf, len);
	return bytes_read;
}

static int write_to_packet_stream(struct rpc_stream *stream, const void *buf, int len)
{
	struct rpc_packet_stream *packet_stream;
	int bytes_written;

	packet_stream = (struct rpc_packet_stream *) stream->ctx;
	bytes_written = rpc_packet_stream_write(packet_stream, buf, len);
	return bytes_written;
}

static int flush_packet_stream(struct rpc_stream *stream)
{
	struct rpc_packet_stream *packet_stream;
	int is_success;

	packet_stream = (struct rpc_packet_stream *) stream->ctx;
	is_success = rpc_packet_stream_flush(packet_stream);
	return is_success;
}

static void disconnect_packet_stream(struct rpc_stream *stream)
{
	struct rpc_packet_stream *packet_stream;

	packet_stream = (struct rpc_packet_stream *) stream->ctx;
	rpc_packet_stream_disconnect(packet_stream);
}

static void delete_packet_stream(struct rpc_stream *stream)
{
	struct rpc_packet_stream *packet_stream;

	packet_stream = (struct rpc_packet_stream *) stream->ctx;
	rpc_packet_stream_delete(packet_stream);
	ff_free(stream);
}

static struct rpc_stream_vtable packet_stream_vtable =
{
	read_from_packet_stream,
	write_to_packet_stream,
	flush_packet_stream,
	disconnect_packet_stream,
	delete_packet_stream
};

struct rpc_stream *rpc_stream_create_from_packet_stream(struct rpc_packet_stream *packet_stream)
{
	struct rpc_stream *stream;

	stream = (struct rpc_stream *) ff_malloc(sizeof(*stream));
	stream->vtalbe = &packet_stream_vtable;
	stream->cxt = packet_stream;

	return stream;
}

typedef void (*rpc_request_processor_release_func)(void *release_func_ctx, struct rpc_request_processor *request_processor, uint8_t request_id);
typedef void (*rpc_request_processor_notify_error_func)(void *notify_error_func_ctx);

struct rpc_request_processor
{
	rpc_request_processor_release_func release_func;
	void *release_func_ctx;
	rpc_request_processor_notify_error_func notify_error_func;
	void *notify_error_func_ctx;
	struct rpc_interface *service_interface;
	void *service_ctx;
	struct rpc_packet_stream *packet_stream;
	struct rpc_stream *stream;
	uint8_t request_id;
};

static void process_request_func(void *ctx)
{
	struct rpc_request_processor *request_processor;
	int is_success;

	request_processor = (struct rpc_request_processor *) ctx;

	is_success = rpc_packet_stream_initialize(request_processor->packet_stream, request_processor->request_id);
	if (is_success)
	{
		is_success = rpc_data_process_next_rpc(request_processor->service_interface, request_processor->service_ctx, request_processor->stream);
		if (!is_success)
		{
			request_processor->notify_error_func(request_processor->notify_error_func_ctx);
		}
		rpc_packet_stream_shutdown(request_processor->packet_stream);
	}
	else
	{
		request_processor->notify_error_func(request_processor->notify_error_func_ctx);
	}
	rpc_packet_stream_clear_reader_queue(request_processor->packet_stream);
	request_processor->release_func(request_processor->release_func_ctx, request_processor, request_processor->request_id);
}

struct rpc_request_processor *rpc_request_processor_create(rpc_request_processor_release_func release_func, void *release_func_ctx,
	rpc_request_processor_notify_error_func notify_error_func, void *notify_error_func_ctx,
	rpc_request_processor_acquire_packet_func acquire_packet_func, rpc_request_processor_release_packet_func release_packet_func, void *packet_func_ctx,
	struct rpc_interface *service_interface, void *service_ctx, struct ff_blocking_queue *writer_queue)
{
	struct rpc_request_processor *request_processor;

	request_processor = (struct rpc_request_processor *) ff_malloc(sizeof(*request_processor));
	request_processor->release_func = release_func;
	request_processor->release_func_ctx = release_func_ctx;
	request_processor->notify_error_func = notify_error_func;
	request_processor->notify_error_func_ctx = notify_error_func_ctx;
	request_processor->service_interface = service_interface;
	request_processor->service_ctx = service_ctx;
	request_processor->packet_stream = rpc_packet_stream_create(writer_queue, acquire_packet_func, release_packet_func, packet_func_ctx);
	request_processor->stream = rpc_stream_create_from_packet_stream(packet_stream);
	request_processor->request_id = 0;
	return request_processor;
}

void rpc_request_processor_delete(struct rpc_request_processor *request_processor)
{
	rpc_stream_delete(request_processor->stream);
	/* there is no need to make the call
	 *   rpc_packet_stream_delete(request_processor->packet_stream);
	 * here, because it was already called by rpc_stream_delete()
	 */
	ff_free(request_processor);
}

void rpc_request_processor_start(struct rpc_request_processor *request_processor, uint8_t request_id)
{
	request_processor->request_id = request_id;
	ff_core_fiberpool_execute_async(process_request_func, request_processor);
}

void rpc_request_processor_stop_async(struct rpc_request_processor *request_processor)
{
	rpc_stream_disconnect(request_processor->stream);
}

void rpc_request_processor_push_packet(struct rpc_request_processor *request_processor, struct rpc_packet *packet)
{
	rpc_packet_stream_push_packet(request_processor->packet_stream, packet);
}

#define MAX_REQUEST_STREAMS_CNT 0x100
#define MAX_WRITER_QUEUE_SIZE 1000
#define MAX_PACKETS_CNT 1000

struct rpc_client_stream_processor
{
	struct ff_blocking_queue *writer_queue;
	struct ff_pool *request_streams_pool;
	struct rpc_bitmap *request_streams_bitmap;
	struct ff_event *request_streams_stop_event;
	struct ff_pool *packets_pool;
	struct rpc_request_stream *request_streams[MAX_REQUEST_STREAMS_CNT];
	struct rpc_stream *stream;
	int request_streams_cnt;
};

static void *create_request_stream(void *ctx)
{
	struct rpc_client_stream_processor *stream_processor;
	struct rpc_request_stream *request_stream;

	stream_processor = (struct rpc_client_stream_processor *) ctx;
	request_stream = rpc_request_stream_create(stream_processor->writer_queue, stream_processor->request_streams_bitmap);
	return request_stream;
}

static void delete_request_stream(void *ctx)
{
	struct rpc_request_stream *request_stream;

	request_stream = (struct rpc_request_stream *) ctx;
	rpc_request_stream_delete(request_stream);
}

static struct rpc_request_stream *acquire_request_stream(struct rpc_client_stream_processor *stream_processor)
{
	struct rpc_request_stream *request_stream;
	uint8_t request_id;

	ff_assert(stream_processor->request_streams_cnt >= 0);

	request_stream = (struct rpc_request_stream *) ff_pool_acquire_entry(stream_processor->request_streams_pool);
	request_id = rpc_request_stream_get_request_id(request_stream);
	ff_assert(stream_processor->request_streams[request_id] == NULL);
	stream_processor->request_streams[request_id] = request_stream;

	stream_processor->request_streams_cnt++;
	ff_assert(stream_processor->request_streams_cnt <= MAX_REQUEST_STREAMS_CNT);
	if (stream_processor->request_streams_cnt == 1)
	{
		ff_event_reset(stream_processor->request_streams_stop_event);
	}

	return request_stream;
}

static void release_request_stream(struct rpc_client_stream_processor *stream_processor, struct rpc_request_stream *request_stream)
{
	uint8_t request_id;

	ff_assert(stream_processor->request_streams_cnt > 0);
	ff_assert(stream_processor->request_streams_cnt <= MAX_REQUEST_STREAMS_CNT);

	request_id = rpc_request_stream_get_request_id(request_stream);
	ff_assert(stream_processor->request_streams[request_id] == request_stream);
	stream_processor->request_streams[request_id] = NULL;
	ff_pool_release_entry(stream_processor->request_streams_pool, request_stream);

	stream_processor->request_streams_cnt--;
	if (stream_processor->request_streams_cnt == 0)
	{
		ff_event_set(stream_processor->request_streams_stop_event);
	}
}

static void *create_packet(void *ctx)
{
	struct rpc_packet *packet;

	packet = rpc_packet_create();
	return packet;
}

static void delete_packet(void *ctx)
{
	struct rpc_packet *packet;

	packet = (struct rpc_packet *) ctx;
	rpc_packet_delete(packet);
}

static void stop_request_stream(void *entry, void *ctx, int is_acquired)
{
	struct rpc_request_stream *request_stream;

	request_stream = (struct rpc_request_stream *) entry;
	if (is_acquired)
	{
		rpc_request_stream_stop_async(request_stream);
	}
}

static void stop_all_request_streams(struct rpc_client_stream_processor *stream_processor)
{
	ff_pool_for_each_entry(stream_processor->request_streams_pool, stop_request_stream, stream_processor);
	ff_event_wait(stream_processor->request_streams_stop_event);
	ff_assert(stream_processor->request_streams_cnt == 0);
}

static void stream_writer_func(void *ctx)
{
	struct rpc_client_stream_processor *stream_processor;

	stream_processor = (struct rpc_client_stream_processor *) ctx;

	for (;;)
	{
		struct rpc_packet *packet;
		int is_empty;
		int is_success;

		ff_blocking_queue_get(stream_processor->writer_queue, &packet);
		if (packet == NULL)
		{
			is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
			ff_assert(is_empty);
			break;
		}
		is_success = rpc_packet_write_to_stream(packet, stream_processor->stream);
		ff_pool_release_entry(stream_processor->packets_pool, packet);
		is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
		if (is_success && is_empty)
		{
			is_success = rpc_stream_flush(stream_processor->stream);
		}
		if (!is_success)
		{
			rpc_client_stream_processor_stop_async(stream_processor);
			skip_writer_queue_packets(stream_processor);
			break;
		}
	}

	ff_event_set(stream_processor->writer_stop_event);
}

static void start_stream_writer(struct rpc_client_stream_processor *stream_processor)
{
	ff_core_fiberpool_execute_async(stream_writer_func, stream_processor);
}

static void stop_stream_writer(struct rpc_client_stream_processor *stream_processor)
{
	ff_blocking_queue_put(stream_processor->writer_queue, NULL);
	ff_event_wait(stream_processor->writer_stop_event);
}

struct rpc_client_stream_processor *rpc_client_stream_processor_create()
{
	struct rpc_client_stream_processor *stream_processor;
	int i;

	stream_processor = (struct rpc_client_stream_processor *) ff_malloc(sizeof(*stream_processor));
	stream_processor->writer_queue = ff_blocking_queue_create(MAX_WRITER_QUEUE_SIZE);
	stream_processor->request_streams_pool = ff_pool_create(MAX_REQUEST_STREAMS_CNT, create_request_stream, stream_processor, delete_request_stream);
	stream_processor->request_streams_bitmap = rpc_bitmap_create(MAX_REQUEST_STREAMS_CNT);
	stream_processor->request_streams_stop_event = ff_event_create(FF_EVENT_AUTO);
	stream_processor->writer_stop_event = ff_event_create(FF_EVENT_AUTO);
	stream_processor->packets_pool = ff_pool_create(MAX_PACKETS_CNT, create_packet, stream_processor, delete_packet);

	for (i = 0; i < MAX_REQUEST_STREAMS_CNT; i++)
	{
		stream_processor->request_streams[i] = NULL;
	}

	stream_processor->stream = NULL;
	stream_processor->request_streams_cnt = 0;

	return stream_processor;
}

void rpc_client_stream_processor_delete(struct rpc_client_stream_processor *stream_processor)
{
	ff_assert(stream_processor->stream == NULL);
	ff_assert(stream_processor->request_streams_cnt == 0);

	ff_pool_delete(stream_processor->packets_pool);
	ff_event_delete(stream_processor->writer_stop_event);
	ff_event_delete(stream_processor->request_streams_stop_event);
	ff_blocking_queue_delete(stream_processor->writer_queue);
	ff_pool_delete(stream_processor->request_streams_pool);
	rpc_bitmap_delete(stream_processor->request_streams_bitmap);
	ff_free(stream_processor);
}

void rpc_client_stream_processor_process_stream(struct rpc_client_stream_processor *stream_processor, struct rpc_stream *stream)
{
	ff_assert(stream_processor->stream == NULL);
	ff_assert(stream_processor->request_streams_cnt == 0);

	stream_processor->stream = stream;
	start_stream_writer(stream_processor);
	ff_event_set(stream_processor->request_streams_stop_event);

	for (;;)
	{
		struct rpc_packet *packet;
		int is_success;
		uint8_t request_id;
		struct rpc_request_stream *request_stream;

		packet = (struct rpc_packet *) ff_pool_acquire_entry(stream_processor->packets_pool);
		is_success = rpc_packet_read_from_stream(packet, stream);
		if (!is_success)
		{
			ff_pool_release_entry(stream_processor->packets_pool, packet);
			break;
		}

		request_id = rpc_packet_get_request_id(packet);
		request_stream = stream_processor->request_streams[request_id];
		if (request_stream == NULL)
		{
			ff_pool_release_entry(stream_processor->packets_pool, packet);
			break;
		}
		rpc_request_stream_push_packet(request_stream, packet);
	}

	rpc_client_stream_processor_stop_async(stream_processor);
	stop_all_request_streams(stream_processor);
	ff_assert(stream_processor->request_streams_cnt == 0);
	stop_stream_writer(stream_processor);
}

void rpc_client_stream_processor_stop_async(struct rpc_client_stream_processor *stream_processor)
{
	if (stream_processor->stream != NULL)
	{
		rpc_stream_disconnect(stream_processor->stream);
		stream_processor->stream = NULL;
	}
}

int rpc_client_stream_processor_invoke_rpc(struct rpc_client_stream_processor *stream_processor, struct rpc_data *data)
{
	int is_success = 0;

	if (stream_processor->stream != NULL)
	{
		struct rpc_request_stream *request_stream;

		request_stream = acquire_request_stream(stream_processor);
		is_success = rpc_request_stream_invoke_rpc(request_stream, data);
		release_request_stream(stream_processor, request_stream);
	}

	return is_success;
}

#define MAX_REQUEST_PROCESSORS_CNT 0x100
#define MAX_WRITER_QUEUE_SIZE 1000
#define MAX_PACKETS_CNT 1000

typedef void (*rpc_server_stream_processor_release_func)(void *release_func_ctx, struct rpc_server_stream_processor *stream_processor);

struct rpc_server_stream_processor
{
	rpc_server_stream_processor_release_func release_func;
	void *release_func_ctx;
	struct rpc_interface *service_interface;
	void *service_ctx;
	struct ff_event *writer_stop_event;
	struct ff_event *request_processors_stop_event;
	struct ff_pool *request_processors_pool;
	struct ff_pool *packets_pool;
	struct ff_blocking_queue *writer_queue;
	struct rpc_request_processor *request_processors[MAX_REQUEST_PROCESSORS_CNT];
	struct rpc_stream *stream;
	int request_processors_cnt;
};

static void skip_writer_queue_packets(struct rpc_server_stream_processor *stream_processor)
{
	for (;;)
	{
		struct rpc_packet *packet;

		ff_blocking_queue_get(stream_processor->writer_queue, &packet);
		if (packet == NULL)
		{
			int is_empty;

			is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
			ff_assert(is_empty);
			break;
		}
		ff_pool_release_entry(stream_processor->packets_pool, packet);
	}
}

static void stream_writer_func(void *ctx)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;

	for (;;)
	{
		struct rpc_packet *packet;
		int is_empty;
		int is_success;

		ff_blocking_queue_get(stream_processor->writer_queue, &packet);
		if (packet == NULL)
		{
			is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
			ff_assert(is_empty);
			break;
		}
		is_success = rpc_packet_write_to_stream(packet, stream_processor->stream);
		ff_pool_release_entry(stream_processor->packets_pool, packet);
		is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
		if (is_success && is_empty)
		{
			is_success = rpc_stream_flush(stream_processor->stream);
		}
		if (!is_success)
		{
			rpc_server_stream_processor_stop_async(stream_processor);
			skip_writer_queue_packets(stream_processor);
			break;
		}
	}

	ff_event_set(stream_processor->writer_stop_event);
}

static void start_stream_writer(struct rpc_server_stream_processor *stream_processor)
{
	ff_core_fiberpool_execute_async(stream_writer_func, stream_processor);
}

static void stop_stream_writer(struct rpc_server_stream_processor *stream_processor)
{
	ff_blocking_queue_put(stream_processor->writer_queue, NULL);
	ff_event_wait(stream_processor->writer_stop_event);
}

static void stop_request_processor(void *entry, void *ctx, int is_acquired)
{
	struct rpc_request_processor *request_processor;

	request_processor = (struct rpc_request_processor *) entry;
	if (is_acquired)
	{
		rpc_request_processor_stop_async(request_processor);
	}
}

static void stop_all_request_processors(struct rpc_server_stream_processor *stream_processor)
{
	ff_pool_for_each_entry(stream_processor->request_processors_pool, stop_request_processor, stream_processor);
	ff_event_wait(stream_processor->request_processors_stop_event);
	ff_assert(stream_processor->request_processors_cnt == 0);
}

static struct rpc_request_processor *acquire_request_processor(struct rpc_server_stream_processor *stream_processor, uint8_t request_id)
{
	struct rpc_request_processor *request_processor;

	ff_assert(stream_processor->request_processors_cnt >= 0);
	ff_assert(stream_processor->request_processors_cnt < MAX_REQUEST_PROCESSORS_CNT);

	request_processor = (struct rpc_request_processor *) ff_pool_acquire_entry(stream_processor->request_processors_pool);
	ff_assert(stream_processor->request_processors[request_id] == NULL);
	stream_processor->request_processors[request_id] = request_processor;
	stream_processor->request_processors_cnt++;
	if (stream_processor->request_processors_cnt == 1)
	{
		ff_event_reset(stream_processor->request_processors_stop_event);
	}
	return request_processor;
}

static void release_request_processor(void *ctx, struct rpc_request_processor *request_processor, uint8_t request_id)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;

	ff_assert(stream_processor->request_processors_cnt > 0);
	ff_assert(stream_processor->request_processors_cnt <= MAX_REQUEST_PROCESSORS_CNT);
	ff_assert(stream_processor->request_processors[request_id] == request_processor);

	ff_pool_release_entry(stream_processor->request_processors_pool, request_processor);
	stream_processor->request_processors[request_id] = NULL;
	stream_processor->request_processors_cnt--;
	if (stream_processor->request_processors_cnt == 0)
	{
		ff_event_set(stream_processor->request_processors_stop_event);
	}
}

static void notify_request_processor_error(void *ctx)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;
	rpc_server_stream_processor_stop_async(stream_processor);
}

static struct rpc_packet *acquire_packet(void *ctx)
{
	struct rpc_server_stream_processor *stream_processor;
	struct rpc_packet *packet;

	stream_processor = (struct rpc_server_stream_processor *) ctx;
	packet = (struct rpc_packet *) ff_pool_acquire_entry(stream_processor->packets_pool);
	return packet;
}

static void release_packet(void *ctx, struct rpc_packet *packet)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;
	rpc_packet_reset(packet);
	ff_pool_release_entry(stream_processor->packets_pool, packet);
}

static void *create_request_processor(void *ctx)
{
	struct rpc_server_stream_processor *stream_processor;
	struct rpc_request_processor *request_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;
	request_processor = rpc_request_processor_create(release_request_processor, stream_processor, notify_request_processor_error, stream_processor,
		acquire_packet, release_packet, stream_processor,
		stream_processor->service_interface, stream_processor->service_ctx, stream_processor->writer_queue);
	return request_processor;
}

static void delete_request_processor(void *ctx)
{
	struct rpc_request_processor *request_processor;

	request_processor = (struct rpc_request_processor *) ctx;
	rpc_request_processor_delete(request_processor);
}

static void *create_packet(void *ctx)
{
	struct rpc_packet *packet;

	packet = rpc_packet_create();
	return packet;
}

static void delete_packet(void *ctx)
{
	struct rpc_packet *packet;

	packet = (struct rpc_packet *) ctx;
	rpc_packet_delete(packet);
}

static void stream_reader_func(void *ctx)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;

	ff_assert(stream_processor->request_processors_cnt == 0);
	ff_assert(stream_processor->stream != NULL);

	start_stream_writer(stream_processor);
	ff_event_set(stream_processor->request_processors_stop_event);
	for (;;)
	{
		struct rpc_packet *packet;
		int is_success;
		enum rpc_packet_type packet_type;
		uint8_t request_id;
		struct rpc_request_processor *request_processor;

		packet = (struct rpc_packet *) ff_pool_acquire_entry(stream_processor->packets_pool);
		is_success = rpc_packet_read_from_stream(packet, stream_processor->stream);
		if (!is_success)
		{
			ff_pool_release_entry(stream_processor->packets_pool, packet);
			break;
		}

		packet_type = rpc_packet_get_type(packet);
		request_id = rpc_packet_get_request_id(packet);
		request_processor = stream_processor->request_processors[request_id];
		if (packet_type == RPC_PACKET_FIRST || packet_type == RPC_PACKET_SINGLE)
		{
			if (request_processor != NULL)
			{
				ff_pool_release_entry(stream_processor->packets_pool, packet);
				break;
			}

			request_processor = acquire_request_processor(stream_processor, request_id);
			rpc_request_processor_start(request_processor, request_id);
		}
		else
		{
			/* packet_type is RPC_PACKET_MIDDLE or RPC_PACKET_LAST */
			if (request_processor == NULL)
			{
				ff_pool_release_entry(stream_processor->packets_pool, packet);
				break;
			}
		}
		rpc_request_processor_push_packet(request_processor, packet);
	}
	rpc_server_stream_processor_stop_async(stream_processor);
	stop_all_request_processors(stream_processor);
	stop_stream_writer(stream_processor);
	rpc_stream_delete(stream_processor->stream);
	stream_processor->stream = NULL;
	stream_processor->release_func(release_func_ctx, stream_processor);
}

struct rpc_server_stream_processor *rpc_server_stream_processor_create(rpc_server_stream_processor_release_func release_func,
	void *release_func_ctx, struct rpc_interface *service_interface, void *service_ctx)
{
	struct rpc_server_stream_processor *stream_processor;
	int i;

	stream_processor = (struct rpc_server_stream_processor *) ff_malloc(sizeof(*stream_processor));
	stream_processor->release_func = release_func;
	stream_processor->release_func_ctx = release_func_ctx;
	stream_processor->service_interface = service_interface;
	stream_processor->service_ctx = service_ctx;
	stream_processor->writer_stop_event = ff_event_create(FF_EVENT_AUTO);
	stream_processor->request_processors_stop_event = ff_event_create(FF_EVENT_AUTO);
	stream_processor->request_processors = ff_pool_create(MAX_REQUEST_PROCESSORS_CNT, create_request_processor, stream_processor, delete_request_processor);
	stream_processor->packets_pool = ff_pool_create(MAX_PACKETS_CNT, create_packet, stream_processor, delete_packet);
	stream_processor->writer_queue = ff_blocking_queue_create(MAX_WRITER_QUEUE_SIZE);

	for (i = 0; i < MAX_REQUEST_PROCESSORS_CNT; i++)
	{
		stream_processor->request_processors[i] = NULL;
	}

	stream_processor->stream = NULL;
	stream_processor->request_processors_cnt = 0;
}

void rpc_server_stream_processor_delete(struct rpc_server_stream_processor *stream_processor)
{
	ff_assert(stream_processor->stream == NULL);
	ff_assert(stream_processor->request_processors_cnt == 0);

	ff_blocking_queue_delete(stream_processor->writer_queue);
	ff_pool_delete(stream_processor->packets_pool);
	ff_pool_delete(stream_processor->request_processors);
	ff_event_delete(stream_processor->request_processors_stop_event);
	ff_event_delete(stream_processor->writer_stop_event);
	ff_free(stream_processor);
}

void rpc_server_stream_processor_start(struct rpc_server_stream_processor *stream_processor, struct rpc_stream *stream)
{
	ff_assert(stream_processor->stream == NULL);
	ff_assert(stream != NULL);

	stream_processor->stream = stream;
	ff_core_fiberpool_execute_async(stream_reader_func, stream_processor);
}

void rpc_server_stream_processor_stop_async(struct rpc_server_stream_processor *stream_processor)
{
	ff_assert(stream_processor->stream != NULL);

	rpc_stream_disconnect(stream_processor->stream);
}

#define RECONNECT_TIMEOUT 500
#define RPC_INVOKATION_TIMEOUT 2000

struct rpc_client
{
	struct ff_arch_net_addr *service_addr;
	struct ff_event *stop_event;
	struct ff_event *must_shutdown_event;
	struct rpc_client_stream_processor *stream_processor;
};

static void main_client_func(void *ctx)
{
	struct rpc_client *client;

	client = (struct rpc_client *) ctx;
	for (;;)
	{
		struct ff_tcp *service_tcp;
		int is_success;

		service_tcp = ff_tcp_create();
		is_success = ff_tcp_connect(service_tcp, client->service_addr);
		if (is_success)
		{
			struct rpc_stream *service_stream;

			service_stream = rpc_stream_create_from_tcp(service_tcp);
			rpc_client_stream_processor_process_stream(client->stream_processor, service_stream);
			rpc_stream_delete(service_stream);
			/* there is no need to call ff_tcp_delete(service_tcp) here,
			 * because the rpc_stream_delete(service_stream) already called this function
			 */
		}
		else
		{
			ff_tcp_delete(service_tcp);
		}

		is_success = ff_event_wait_with_timeout(client->must_shutdown_event, RECONNECT_TIMEOUT);
		if (is_success)
		{
			/* the rpc_client_delete() was called */
			break;
		}
	}
	ff_event_set(client->stop_event);
}

static void start_client(struct rpc_client *client)
{
	ff_core_fiberpool_execute_async(main_client_func, client);
}

static void stop_client(struct rpc_client *client)
{
	ff_event_set(client->must_shutdown_event);
	rpc_client_stream_processor_stop_async(client->stream_processor);
	ff_event_wait(client->stop_event);
}

struct rpc_client *rpc_client_create(struct ff_arch_net_addr *service_addr)
{
	struct rpc_client *client;

	ff_assert(service_interface != NULL);
	ff_assert(service_addr != NULL);

	client = (struct rpc_client *) ff_malloc(sizeof(*client));
	client->service_addr = service_addr;
	client->stop_event = ff_event_create(FF_EVENT_AUTO);
	client->must_shutdown_event = ff_event_create(FF_EVENT_AUTO);
	client->stream_processor = rpc_client_stream_processor_create();

	start_client(client);

	return client;
}

void rpc_client_delete(struct rpc_client *client)
{
	ff_assert(client->service_addr != NULL);

	stop_client(client);

	rpc_client_stream_processor_delete(client->stream_processor);
	ff_event_delete(client->must_shutdown_event);
	ff_event_delete(client->stop_event);
	ff_free(client);
}

int rpc_client_invoke_rpc(struct rpc_client *client, struct rpc_data *data)
{
	int is_success;

	is_success = rpc_client_stream_processor_invoke_rpc(client->stream_processor, data);

	return is_success;
}

#define MAX_STREAM_PROCESSORS_CNT 0x100

struct rpc_server
{
	struct rpc_interface *service_interface;
	void *service_ctx;
	struct ff_tcp *accept_tcp;
	struct ff_event *stop_event;
	struct ff_event *stream_processors_stop_event;
	struct ff_pool *stream_processors;
	int stream_processors_cnt;
};

static void stop_stream_processor(void *entry, void *ctx, int is_acquired)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) entry;
	if (is_acquired)
	{
		rpc_server_stream_processor_stop_async(stream_processor);
	}
}

static void stop_all_stream_processors(struct rpc_server *server)
{
	ff_pool_for_each_entry(server->stream_processors, stop_stream_processor, server);
	ff_event_wait(server->stream_processors_stop_event);
	ff_assert(server->stream_processors_cnt == 0);
}

static struct rpc_server_stream_processor *acquire_stream_processor(struct rpc_server *server)
{
	struct rpc_server_stream_processor *stream_processor;

	ff_assert(server->stream_processors_cnt >= 0);
	ff_assert(server->stream_processors_cnt < MAX_STREAM_PROCESSORS_CNT);

	stream_processor = (struct rpc_server_stream_processor *) ff_pool_acquire_entry(server->stream_processors);
	server->stream_processors_cnt++;
	if (server->stream_processors_cnt == 1)
	{
		ff_event_reset(server->stream_processors_stop_event);
	}
	return stream_processor;
}

static void release_stream_processor(void *ctx, struct rpc_server_stream_processor *stream_processor)
{
	struct rpc_server *server;

	server = (struct rpc_server *) ctx;

	ff_assert(server->stream_processors_cnt > 0);
	ff_assert(server->stream_processors <= MAX_STREAM_PROCESSORS_CNT);

	ff_pool_release_entry(server->stream_processors, stream_processor);
	server->stream_processors_cnt--;
	if (server->stream_processors_cnt == 0)
	{
		ff_event_set(server->stream_processors_stop_event);
	}
}

static void *create_stream_processor(void *ctx)
{
	struct rpc_server *server;
	struct rpc_server_stream_processor *stream_processor;

	server = (struct rpc_server *) ctx;
	stream_processor = rpc_server_stream_processor_create(release_stream_processor, server, server->service_interface, server->service_ctx);
	return stream_processor;
}

static void delete_stream_processor(void *ctx)
{
	struct rpc_server_stream_processor *stream_processor;

	stream_processor = (struct rpc_server_stream_processor *) ctx;
	rpc_server_stream_processor_delete(stream_processor);
}

static void main_server_func(void *ctx)
{
	struct rpc_server *server;
	struct ff_arch_net_addr *remote_addr;

	server = (struct rpc_server *) ctx;

	ff_assert(server->stream_processors_cnt == 0);

	remote_addr = ff_arch_net_addr_create();
	ff_event_set(server->stream_processors_stop_event);
	for (;;)
	{
		struct ff_tcp *client_tcp;
		struct rpc_server_stream_processor *stream_processor;
		struct rpc_stream *client_stream;

		client_tcp = ff_tcp_accept(server->accept_tcp, remote_addr);
		if (client_tcp == NULL)
		{
			break;
		}

		stream_processor = acquire_stream_processor(server);

		client_stream = rpc_stream_create_from_tcp(client_tcp);
		rpc_server_stream_processor_start(stream_processor, client_stream);
	}
	stop_all_stream_processors(server);
	ff_arch_net_addr_delete(remote_addr);

	ff_event_set(server->stop_event);
}

static void start_server(struct rpc_server *server)
{
	ff_core_fiberpool_execute_async(main_server_func, server);
}

static void stop_server(struct rpc_server *server)
{
	ff_tcp_disconnect(server->accept_tcp);
	ff_event_wait(server->stop_event);
	ff_assert(server->stream_processors_cnt == 0);
}

struct rpc_server *rpc_server_create(struct rpc_interface *service_interface, void *service_ctx, struct ff_arch_net_addr *listen_addr)
{
	struct rpc_server *server;
	int is_success;

	server = (struct rpc_server *) ff_malloc(sizeof(*server));
	server->service_interface = service_interface;
	server->service_ctx = service_ctx;
	server->accept_tcp = ff_tcp_create();
	is_success = ff_tcp_bind(server->accept_tcp, params->listen_addr, FF_TCP_SERVER);
	if (!is_success)
	{
		const wchar_t *addr_str;

		addr_str = ff_arch_net_addr_to_string(params->listen_addr);
		ff_log_fatal_error(L"cannot bind the address %ls to the server", addr_str);
		/* there is no need to call ff_arch_net_add_delete_string() here,
		 * because the ff_log_fatal_error() will terminate the application
		 */
	}
	server->stop_event = ff_event_create(FF_EVENT_AUTO);
	server->stream_processors_stop_event = ff_event_create(FF_EVENT_AUTO);
	server->stream_processors = ff_pool_create(MAX_STREAM_PROCESSORS_CNT, create_stream_processor, server, delete_stream_processor);
	server->stream_processors_cnt = 0;

	start_server(server);

	return server;
}

void rpc_server_delete(struct rpc_server *server)
{
	stop_server(server);

	ff_pool_delete(server->stream_processors);
	ff_event_delete(server->stream_processors_stop_event);
	ff_event_delete(server->stop_event);
	ff_tcp_delete(server->accept_tcp);
	ff_free(server);
}

service_ctx = foo_service_create();

server = rpc_server_create(foo_interface_extern, service_ctx, listen_addr);
wait();
rpc_server_delete(server);


static const struct rpc_param_vtable *foo_request_param_vtables[] =
{
	&rpc_uint32_param_vtable,
	&rpc_int64_param_vtable,
	&rpc_blob_param_vtable
};

static const struct rpc_param_vtable *foo_response_param_vtables[] =
{
	&rpc_int32_param_vtable
};

static void foo_callback(struct rpc_data *data, void *service_ctx)
{
	uint32_t a;
	int64_t b;
	struct rpc_blob *c;
	int32_t d;

	rpc_data_get_request_param_value(data, 0, &a);
	rpc_data_get_request_param_value(data, 1, &b);
	rpc_data_get_request_param_value(data, 2, &c);

	service_func_foo(service_ctx, a, b, c, &d);

	rpc_data_set_response_param_value(data, 0, &d);
}

static const struct rpc_method foo_method =
{
	foo_callback,
	&foo_request_param_vtables,
	&foo_response_param_vtables,
	NULL,
	foo_request_params_cnt,
	foo_response_params_cnt
};

static const struct rpc_method *interface_methods[] =
{
	&foo_method,
	&bar_method
};

static const struct rpc_interface interface =
{
	interface_methods,
	interface_methods_cnt
};

extern const struct rpc_interface *foo_interface_extern = &interface;

struct rpc_param_vtable
{
	void *(*create)();
	void (*delete)(void *param);
	int (*read)(void *param, struct rpc_stream *stream);
	int (*write)(const void *param, struct rpc_stream *stream);
	void (*get_value)(const void *param, void **value);
	void (*set_value)(void *param, const void *value);
	uint32_t (*get_hash)(void *param, uint32_t start_value);
};

typedef void (*rpc_method_callback)(struct rpc_data *data, void *service_ctx);

struct rpc_method
{
	rpc_method_callback callback;
	struct rpc_param_vtable **request_param_vtables;
	struct rpc_param_vtable **response_param_vtables;
	int *is_key;
	int request_params_cnt;
	int response_params_cnt;
};

#define MAX_PARAMS_CNT 100

static void **create_params(struct rpc_param_vtable **param_vtables, int param_cnt)
{
	int i;
	void **params;

	ff_assert(param_cnt >= 0);
	ff_assert(param_cnt < MAX_PARAMS_CNT);
	params = (void **) ff_malloc(sizeof(*params) * param_cnt);
	for (i = 0; i < param_cnt; i++)
	{
		struct rpc_param_vtable *vtable;
		void *param;

		vtable = param_vtables[i];
		param = vtable->create();
		params[i] = param;
	}

	return params;
}

static void delete_params(void **params, struct rpc_param_vtable **param_vtables, int param_cnt)
{
	int i;

	ff_assert(param_cnt >= 0);
	ff_assert(param_cnt < MAX_PARAMS_CNT);
	for (i = 0; i < param_cnt; i++)
	{
		struct rpc_param_vtable *vtable;
		void *param;

		vtable = param_vtables[i];
		param = params[i];
		vtable->delete(param);
	}
	ff_free(params);
}

static int read_params(void **params, struct rpc_param_vtable *param_vtables, int param_cnt, struct rpc_stream *stream)
{
	int i;
	int is_success = 1;

	ff_assert(param_cnt >= 0);
	ff_assert(param_cnt < MAX_PARAMS_CNT);
	for (i = 0; i < param_cnt; i++)
	{
		struct rpc_param_vtable *vtable;
		void *param;

		vtable = param_vtables[i];
		param = params[i];
		is_success = vtable->read(param, stream);
		if (!is_success)
		{
			break;
		}
	}

	return is_success;
}

static int write_params(void **params, struct rpc_param_vtable *param_vtables, int param_cnt, struct rpc_stream *stream)
{
	int i;
	int is_success = 1;

	ff_assert(param_cnt >= 0);
	ff_assert(param_cnt < MAX_PARAMS_CNT);
	for (i = 0; i < param_cnt; i++)
	{
		struct rpc_param_vtable *vtable;
		void *param;

		vtable = param_vtables[i];
		param = params[i];
		is_success = vtable->write(param, stream);
		if (!is_success)
		{
			break;
		}
	}

	return is_success;
}

static void get_param_value(void **params, void **value, int param_idx, struct rpc_param_vtable *param_vtables, int params_cnt)
{
	struct rpc_param_vtable *vtable;
	void *param;

	ff_assert(params_cnt >= 0);
	ff_assert(params_cnt < MAX_PARAMS_CNT);
	ff_assert(param_idx >= 0);
	ff_assert(param_idx < params_cnt);

	vtable = param_vtables[param_idx];
	param = params[param_idx];
	vtable->get_value(param, value);
}

static void set_param_value(void **params, const void *value, int param_idx, struct rpc_param_vtable *param_vtables, int params_cnt)
{
	struct rpc_param_vtable *vtable;
	void *param;

	ff_assert(params_cnt >= 0);
	ff_assert(params_cnt < MAX_PARAMS_CNT);
	ff_assert(param_idx >= 0);
	ff_assert(param_idx < params_cnt);

	vtable = param_vtables[param_idx];
	param = params[param_idx];
	vtable->set_value(param, value);
}

void rpc_method_create_params(struct rpc_method *method, void ***request_params, void ***response_params)
{
	*request_params = create_params(method->request_param_vtables, method->request_params_cnt);
	*response_params = create_params(method->response_param_vtables, method->response_params_cnt);
}

void rpc_method_delete_params(struct rpc_method *method, void **request_params, void **response_params)
{
	delete_params(request_params, method->request_param_vtables, method->request_params_cnt);
	delete_params(response_params, method->response_param_vtables, method->response_params_cnt);
}

int rpc_method_read_request_params(struct rpc_method *method, void **params, struct rpc_stream *stream)
{
	int is_success;

	is_success = read_params(params, method->request_param_vtables, method->request_params_cnt, stream);
	return is_success;
}

int rpc_method_read_response_params(struct rpc_method *method, void **params, struct rpc_stream *stream)
{
	int is_success;

	is_success = read_params(params, method->response_param_vtables, method->response_params_cnt, stream);
	return is_success;
}

int rpc_method_write_request_params(struct rpc_method *method, void **params, struct rpc_stream *stream)
{
	int is_success;

	is_success = write_params(params, method->request_param_vtables, method->request_params_cnt, stream);
	return is_success;
}

int rpc_method_write_response_params(struct rpc_method *method, void **params, struct rpc_stream *stream)
{
	int is_success;

	is_success = write_params(params, method->response_param_vtables, method->response_params_cnt, stream);
	return is_success;
}

void rpc_method_set_request_param_value(struct rpc_method *method, int param_idx, void **params, const void *value)
{
	set_param_value(params, value, param_idx, method->request_param_vtables, method->request_params_cnt);
}

void rpc_method_get_request_param_value(struct rpc_method *method, int param_idx, void **params, void **value)
{
	get_param_value(params, value, param_idx, method->request_param_vtables, method->request_params_cnt);
}

void rpc_method_set_response_param_value(struct rpc_method *method, int param_idx, void **params, const void *value)
{
	set_param_value(params, value, param_idx, method->response_param_vtables, method->response_params_cnt);
}

void rpc_method_get_response_param_value(struct rpc_method *method, int param_idx, void **params, void **value)
{
	get_param_value(params, value, param_idx, method->response_param_vtables, method->response_params_cnt);
}

uint32_t rpc_method_get_request_hash(struct rpc_method *method, uint32_t start_value, void **params)
{
	int params_cnt;
	uint32_t hash_value;

	hash_value = start_value
	params_cnt = method->request_params_cnt;
	for (i = 0; i < params_cnt; i++)
	{
		int is_key;

		is_key = method->is_key[i];
		if (is_key)
		{
			struct rpc_param_vtable *vtable;
			void *param;

			vtable = method->request_param_vtables[i];
			param = params[i];
			hash_value = vtable->get_hash(param, hash_value);
		}
	}

	return hash_value;
}

void rpc_method_invoke_callback(struct rpc_method *method, struct rpc_data *data, void *service_ctx)
{
	method->callback(data, service_ctx);
}

struct rpc_interface
{
	struct rpc_method **methods;
	int methods_cnt;
};

struct rpc_method *rpc_interface_get_method(struct rpc_interface *interface, uint8_t method_id)
{
	struct rpc_method *method = NULL;

	if (method_id >= 0 && method_id < serivce->methods_cnt)
	{
		method = interface->methods[method_id];
	}
	return method;
}

struct rpc_data
{
	struct rpc_method *method;
	void **request_params;
	void **response_params;
	uint8_t method_id;
};

static struct rpc_data *read_request(struct rpc_interface *interface, struct rpc_stream *stream)
{
	uint8_t method_id;
	int bytes_read;
	struct rpc_method *method;
	struct rpc_data *data = NULL;
	int is_success;

	bytes_read = rpc_stream_read(stream, &method_id, 1);
	if (bytes_read != 1)
	{
		goto end;
	}
	method = rpc_interface_get_method(interface, method_id);
	if (method == NULL)
	{
		goto end;
	}
	data = rpc_data_create(method, method_id);
	is_success = rpc_method_read_request_params(data->method, data->request_params, stream);
	if (!is_success)
	{
		rpc_data_delete(data);
		data = NULL;
	}

end:
	return data;
}

static int write_response(struct rpc_data *data, struct rpc_stream *stream)
{
	int is_success;

	is_success = rpc_method_write_response_params(data->method, data->response_params, stream);
	if (is_success)
	{
		is_success = rpc_stream_flush(stream);
	}
	return is_success;
}

static int write_request(struct rpc_data *data, struct rpc_stream *stream)
{
	int bytes_written;
	int is_success = 0;

	bytes_written = rpc_stream_write(stream, &data->method_id, 1);
	if (bytes_written == 1)
	{
		is_succes = rpc_method_write_request_params(data->method, data->request_params, stream);
		if (is_success)
		{
			is_success = rpc_stream_flush(stream);
		}
	}

	return is_success;
}

static int read_response(struct rpc_data *data, struct rpc_stream *stream)
{
	int is_success;

	is_success = rpc_method_read_response_params(data->method, data->response_params, stream);
	return is_success;
}

struct rpc_data *rpc_data_create(struct rpc_method *method, uint8_t method_id)
{
	struct rpc_data *data;
	void **request_params;
	void **response_params;

	ff_assert(method_id >= 0);
	ff_assert(method_id < MAX_METHODS_CNT);
	rpc_method_create_params(method, &request_params, &response_params);

	data = (struct rpc_data *) ff_malloc(sizeof(*data));
	data->method = method;
	data->request_params = request_params;
	data->response_params = response_params;
	data->method_id = method_id;

	return data;
}

void rpc_data_delete(struct rpc_data *data)
{
	rpc_method_delete_params(data->method, data->request_params, data->response_params);
	ff_free(data);
}

int rpc_data_process_next_rpc(struct rpc_interface *interface, void *service_ctx, struct rpc_stream *stream)
{
	struct rpc_data *data;
	int is_success = 0;

	data = read_request(interface, stream);
	if (data != NULL)
	{
		rpc_method_invoke_callback(data->method, data, service_ctx);
		is_success = write_response(data, stream);
		rpc_data_delete(data);
    }

    return is_success;
}

int rpc_data_invoke_remote_call(struct rpc_data *data, struct rpc_stream *stream)
{
	int is_success;

	is_success = write_request(data, stream);
	if (is_success)
	{
		is_success = read_response(data, stream);
	}

	return is_success;
}

void rpc_data_get_request_param_value(struct rpc_data *data, int param_idx, void **value)
{
	rpc_method_get_request_param_value(data->method, param_idx, data->request_params, value);
}

void rpc_data_set_response_param_value(struct rpc_data *data, int param_idx, const void *value)
{
	rpc_method_set_response_param_value(data->method, param_idx, data->response_params, value);
}

void rpc_data_get_response_param_value(struct rpc_data *data, int param_idx, void **value)
{
	rpc_method_get_response_param_value(data->method, param_idx, data->response_params, value);
}

void rpc_data_set_request_param_value(struct rpc_data *data, int param_idx, const void *value)
{
	rpc_method_set_request_param_value(data->method, param_idx, data->request_params, value);
}

uint32_t rpc_data_get_request_hash(struct rpc_data *data, uint32_t start_value)
{
	uint32_t hash;

	hash = rpc_method_get_request_hash(data->method, start_value, data->request_params);
	return hash;
}