#include <stdio.h>
#define BUF_SIZE 10
typedef struct {
	char *read;
	char *write;
	char buf[BUF_SIZE];
} ringbuffer_t;

#define print_char(val) printf(#val " = %c\n", val)
/* static ringbuffer_t ring_buffer = { */
/* 	.read = ring_buffer.buf, */
/* 	.write = ring_buffer.buf, */
/* 	.buf = { 0 }, */
/* }; */

#define DECLARE_RINGBUFFER(name, buffer_size) \
	ringbuffer_t name = {                 \
		.read = name.buf,             \
		.write = name.buf,            \
		.buf = { 0 },                 \
	};

DECLARE_RINGBUFFER(ring_buffer, BUF_SIZE);
void ringbuffer_write(ringbuffer_t *ring_buffer, char value)
{
	*ring_buffer->write = value;
	if (ring_buffer->write - ring_buffer->buf >= BUF_SIZE) {
		printf("write resetting to buf\n");
		ring_buffer->write = ring_buffer->buf;
	} else {
		ring_buffer->write++;
	}
}

char ringbuffer_read(ringbuffer_t *ring_buffer)
{
	char rv = *ring_buffer->read;
	if (ring_buffer->read - ring_buffer->buf >= BUF_SIZE) {
		printf("read resetting to buf\n");
		ring_buffer->read = ring_buffer->buf;
	} else {
		ring_buffer->read++;
	}
	return rv;
}

void print_buffer(ringbuffer_t *ring_buffer)
{
	ring_buffer->read = ring_buffer->buf;
	for (int i = 0; i < BUF_SIZE; ++i) {
		print_char(ringbuffer_read(ring_buffer));
	}
}

int main()
{
	for (char i = 'a'; i <= 'z'; i++) {
		ringbuffer_write(&ring_buffer, i);
		print_char(ringbuffer_read(&ring_buffer));
	}

	printf("*****************************************\n");
	print_buffer(&ring_buffer);
}
