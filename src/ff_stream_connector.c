#include "private/ff_common.h"

#include "private/ff_stream_connector.h"
#include "private/ff_stream.h"

struct ff_stream_connector
{
	const struct ff_stream_connector_vtable *vtable;
	void *ctx;
};

struct ff_stream_connector *ff_stream_connector_create(const struct ff_stream_connector_vtable *vtable, void *ctx)
{
	struct ff_stream_connector *stream_connector;

	stream_connector = (struct ff_stream_connector *) ff_malloc(sizeof(*stream_connector));
	stream_connector->vtable = vtable;
	stream_connector->ctx = ctx;

	return stream_connector;
}

void ff_stream_connector_delete(struct ff_stream_connector *stream_connector)
{
	stream_connector->vtable->delete(stream_connector->ctx);
	ff_free(stream_connector);
}

void ff_stream_connector_initialize(struct ff_stream_connector *stream_connector)
{
	stream_connector->vtable->initialize(stream_connector->ctx);
}

void ff_stream_connector_shutdown(struct ff_stream_connector *stream_connector)
{
	stream_connector->vtable->shutdown(stream_connector->ctx);
}

struct ff_stream *ff_stream_connector_connect(struct ff_stream_connector *stream_connector)
{
	struct ff_stream *stream;

	stream = stream_connector->vtable->connect(stream_connector->ctx);
	if (stream == NULL)
	{
		ff_log_debug(L"cannot establish connection using stream_connector=%p. See previous messages for more info", stream_connector);
	}
	return stream;
}
