/*
 * sgen-protocol.c: Binary protocol of internal activity, to aid
 * debugging.
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright (C) 2012 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#ifdef HAVE_SGEN_GC

#include "config.h"
#include "sgen-conf.h"
#include "sgen-gc.h"
#include "sgen-protocol.h"
#include "sgen-memory-governor.h"
#include "sgen-thread-pool.h"
#include "sgen-client.h"
#include "mono/utils/mono-membar.h"
#include "mono/utils/mono-proclib.h"

#include <errno.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#include <fcntl.h>
#endif

/* FIXME Implement binary protocol IO on systems that don't have unistd */
#ifdef HAVE_UNISTD_H
/* If valid, dump binary protocol to this file */
static int binary_protocol_file = -1;

/* We set this to -1 to indicate an exclusive lock */
static volatile int binary_protocol_use_count = 0;

#define BINARY_PROTOCOL_BUFFER_SIZE	(65536 - 2 * 8)

typedef struct _BinaryProtocolBuffer BinaryProtocolBuffer;
struct _BinaryProtocolBuffer {
	BinaryProtocolBuffer * volatile next;
	volatile int index;
	unsigned char buffer [BINARY_PROTOCOL_BUFFER_SIZE];
};

static BinaryProtocolBuffer * volatile binary_protocol_buffers = NULL;

static char* filename_or_prefix = NULL;
static int current_file_index = 0;
static long long current_file_size = 0;
static long long file_size_limit;

static char*
filename_for_index (int index)
{
	char *filename;

	SGEN_ASSERT (0, file_size_limit > 0, "Indexed binary protocol filename must only be used with file size limit");

	filename = (char *)sgen_alloc_internal_dynamic (strlen (filename_or_prefix) + 32, INTERNAL_MEM_BINARY_PROTOCOL, TRUE);
	sprintf (filename, "%s.%d", filename_or_prefix, index);

	return filename;
}

static void
free_filename (char *filename)
{
	SGEN_ASSERT (0, file_size_limit > 0, "Indexed binary protocol filename must only be used with file size limit");

	sgen_free_internal_dynamic (filename, strlen (filename_or_prefix) + 32, INTERNAL_MEM_BINARY_PROTOCOL);
}

static void
binary_protocol_open_file (gboolean assert_on_failure)
{
	char *filename;
#ifdef F_SETLK
	struct flock lock;
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
#endif

	if (file_size_limit > 0)
		filename = filename_for_index (current_file_index);
	else
		filename = filename_or_prefix;

	do {
		binary_protocol_file = open (filename, O_CREAT | O_WRONLY, 0644);
		if (binary_protocol_file == -1) {
			if (errno != EINTR)
				break; /* Failed */
#ifdef F_SETLK
		} else if (fcntl (binary_protocol_file, F_SETLK, &lock) == -1) {
			/* The lock for the file is already taken. Fail */
			close (binary_protocol_file);
			binary_protocol_file = -1;
			break;
#endif
		} else {
			/* We have acquired the lock. Truncate the file */
			ftruncate (binary_protocol_file, 0);
		}
	} while (binary_protocol_file == -1);

	if (binary_protocol_file == -1 && assert_on_failure)
		g_error ("sgen binary protocol: failed to open file");

	if (file_size_limit > 0)
		free_filename (filename);
}
#endif

void
binary_protocol_init (const char *filename, long long limit)
{
#ifdef HAVE_UNISTD_H
	file_size_limit = limit;

	/* Original name length + . + pid length in hex + null terminator */
	filename_or_prefix = g_strdup_printf ("%s", filename);
	binary_protocol_open_file (FALSE);

	if (binary_protocol_file == -1) {
		/* Another process owns the file, try adding the pid suffix to the filename */
		gint32 pid = mono_process_current_pid ();
		g_free (filename_or_prefix);
		filename_or_prefix = g_strdup_printf ("%s.%x", filename, pid);
		binary_protocol_open_file (TRUE);
	}

	/* If we have a file size limit, we might need to open additional files */
	if (file_size_limit == 0)
		g_free (filename_or_prefix);

	binary_protocol_header (PROTOCOL_HEADER_CHECK, PROTOCOL_HEADER_VERSION, SIZEOF_VOID_P, G_BYTE_ORDER == G_LITTLE_ENDIAN);
#else
	g_error ("sgen binary protocol: not supported");
#endif
}

gboolean
binary_protocol_is_enabled (void)
{
#ifdef HAVE_UNISTD_H
	return binary_protocol_file != -1;
#else
	return FALSE;
#endif
}

#ifdef HAVE_UNISTD_H

static void
close_binary_protocol_file (void)
{
	while (close (binary_protocol_file) == -1 && errno == EINTR)
		;
	binary_protocol_file = -1;
}

static gboolean
try_lock_exclusive (void)
{
	do {
		if (binary_protocol_use_count)
			return FALSE;
	} while (InterlockedCompareExchange (&binary_protocol_use_count, -1, 0) != 0);
	mono_memory_barrier ();
	return TRUE;
}

static void
unlock_exclusive (void)
{
	mono_memory_barrier ();
	SGEN_ASSERT (0, binary_protocol_use_count == -1, "Exclusively locked count must be -1");
	if (InterlockedCompareExchange (&binary_protocol_use_count, 0, -1) != -1)
		SGEN_ASSERT (0, FALSE, "Somebody messed with the exclusive lock");
}

static void
lock_recursive (void)
{
	int old_count;
	do {
	retry:
		old_count = binary_protocol_use_count;
		if (old_count < 0) {
			/* Exclusively locked - retry */
			/* FIXME: short back-off */
			goto retry;
		}
	} while (InterlockedCompareExchange (&binary_protocol_use_count, old_count + 1, old_count) != old_count);
	mono_memory_barrier ();
}

static void
unlock_recursive (void)
{
	int old_count;
	mono_memory_barrier ();
	do {
		old_count = binary_protocol_use_count;
		SGEN_ASSERT (0, old_count > 0, "Locked use count must be at least 1");
	} while (InterlockedCompareExchange (&binary_protocol_use_count, old_count - 1, old_count) != old_count);
}

static void
binary_protocol_flush_buffer (BinaryProtocolBuffer *buffer)
{
	ssize_t ret;
	size_t to_write = buffer->index;
	size_t written = 0;
	g_assert (buffer->index > 0);

	while (written < to_write) {
		ret = write (binary_protocol_file, buffer->buffer + written, to_write - written);
		if (ret >= 0)
			written += ret;
		else if (errno == EINTR)
			continue;
		else
			close_binary_protocol_file ();
	}

	current_file_size += buffer->index;

	sgen_free_os_memory (buffer, sizeof (BinaryProtocolBuffer), SGEN_ALLOC_INTERNAL);
}

static void
binary_protocol_check_file_overflow (void)
{
	if (file_size_limit <= 0 || current_file_size < file_size_limit)
		return;

	close_binary_protocol_file ();

	if (current_file_index > 0) {
		char *filename = filename_for_index (current_file_index - 1);
		unlink (filename);
		free_filename (filename);
	}

	++current_file_index;
	current_file_size = 0;

	binary_protocol_open_file (TRUE);
}
#endif

/*
 * Flushing buffers takes an exclusive lock, so it must only be done when the world is
 * stopped, otherwise we might end up with a deadlock because a stopped thread owns the
 * lock.
 *
 * The protocol entries that do flush have `FLUSH()` in their definition.
 */
gboolean
binary_protocol_flush_buffers (gboolean force)
{
#ifdef HAVE_UNISTD_H
	int num_buffers = 0, i;
	BinaryProtocolBuffer *header;
	BinaryProtocolBuffer *buf;
	BinaryProtocolBuffer **bufs;

	if (binary_protocol_file == -1)
		return FALSE;

	if (!force && !try_lock_exclusive ())
		return FALSE;

	header = binary_protocol_buffers;
	for (buf = header; buf != NULL; buf = buf->next)
		++num_buffers;
	bufs = (BinaryProtocolBuffer **)sgen_alloc_internal_dynamic (num_buffers * sizeof (BinaryProtocolBuffer*), INTERNAL_MEM_BINARY_PROTOCOL, TRUE);
	for (buf = header, i = 0; buf != NULL; buf = buf->next, i++)
		bufs [i] = buf;
	SGEN_ASSERT (0, i == num_buffers, "Binary protocol buffer count error");

	/*
	 * This might be incorrect when forcing, but all bets are off in that case, anyway,
	 * because we're trying to figure out a bug in the debugger.
	 */
	binary_protocol_buffers = NULL;

	for (i = num_buffers - 1; i >= 0; --i) {
		binary_protocol_flush_buffer (bufs [i]);
		binary_protocol_check_file_overflow ();
	}

	sgen_free_internal_dynamic (buf, num_buffers * sizeof (BinaryProtocolBuffer*), INTERNAL_MEM_BINARY_PROTOCOL);

	if (!force)
		unlock_exclusive ();

	return TRUE;
#endif
}

#ifdef HAVE_UNISTD_H
static BinaryProtocolBuffer*
binary_protocol_get_buffer (int length)
{
	BinaryProtocolBuffer *buffer, *new_buffer;
 retry:
	buffer = binary_protocol_buffers;
	if (buffer && buffer->index + length <= BINARY_PROTOCOL_BUFFER_SIZE)
		return buffer;

	new_buffer = (BinaryProtocolBuffer *)sgen_alloc_os_memory (sizeof (BinaryProtocolBuffer), (SgenAllocFlags)(SGEN_ALLOC_INTERNAL | SGEN_ALLOC_ACTIVATE), "debugging memory");
	new_buffer->next = buffer;
	new_buffer->index = 0;

	if (InterlockedCompareExchangePointer ((void**)&binary_protocol_buffers, new_buffer, buffer) != buffer) {
		sgen_free_os_memory (new_buffer, sizeof (BinaryProtocolBuffer), SGEN_ALLOC_INTERNAL);
		goto retry;
	}

	return new_buffer;
}
#endif

static void
protocol_entry (unsigned char type, gpointer data, int size)
{
#ifdef HAVE_UNISTD_H
	int index;
	BinaryProtocolBuffer *buffer;

	if (binary_protocol_file == -1)
		return;

	if (sgen_thread_pool_is_thread_pool_thread (mono_native_thread_id_get ()))
		type |= 0x80;

	lock_recursive ();

 retry:
	buffer = binary_protocol_get_buffer (size + 1);
 retry_same_buffer:
	index = buffer->index;
	if (index + 1 + size > BINARY_PROTOCOL_BUFFER_SIZE)
		goto retry;

	if (InterlockedCompareExchange (&buffer->index, index + 1 + size, index) != index)
		goto retry_same_buffer;

	/* FIXME: if we're interrupted at this point, we have a buffer
	   entry that contains random data. */

	buffer->buffer [index++] = type;
	memcpy (buffer->buffer + index, data, size);
	index += size;

	g_assert (index <= BINARY_PROTOCOL_BUFFER_SIZE);

	unlock_recursive ();
#endif
}

#define TYPE_INT int
#define TYPE_LONGLONG long long
#define TYPE_SIZE size_t
#define TYPE_POINTER gpointer
#define TYPE_BOOL gboolean

#define BEGIN_PROTOCOL_ENTRY0(method) \
	void method (void) { \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = NULL; \
		int __size = 0; \
		CLIENT_PROTOCOL_NAME (method) ();
#define BEGIN_PROTOCOL_ENTRY1(method,t1,f1) \
	void method (t1 f1) { \
		PROTOCOL_STRUCT(method) __entry = { f1 }; \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = &__entry; \
		int __size = sizeof (PROTOCOL_STRUCT(method)); \
		CLIENT_PROTOCOL_NAME (method) (f1);
#define BEGIN_PROTOCOL_ENTRY2(method,t1,f1,t2,f2) \
	void method (t1 f1, t2 f2) { \
		PROTOCOL_STRUCT(method) __entry = { f1, f2 }; \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = &__entry; \
		int __size = sizeof (PROTOCOL_STRUCT(method)); \
		CLIENT_PROTOCOL_NAME (method) (f1, f2);
#define BEGIN_PROTOCOL_ENTRY3(method,t1,f1,t2,f2,t3,f3) \
	void method (t1 f1, t2 f2, t3 f3) { \
		PROTOCOL_STRUCT(method) __entry = { f1, f2, f3 }; \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = &__entry; \
		int __size = sizeof (PROTOCOL_STRUCT(method)); \
		CLIENT_PROTOCOL_NAME (method) (f1, f2, f3);
#define BEGIN_PROTOCOL_ENTRY4(method,t1,f1,t2,f2,t3,f3,t4,f4) \
	void method (t1 f1, t2 f2, t3 f3, t4 f4) { \
		PROTOCOL_STRUCT(method) __entry = { f1, f2, f3, f4 }; \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = &__entry; \
		int __size = sizeof (PROTOCOL_STRUCT(method)); \
		CLIENT_PROTOCOL_NAME (method) (f1, f2, f3, f4);
#define BEGIN_PROTOCOL_ENTRY5(method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5) \
	void method (t1 f1, t2 f2, t3 f3, t4 f4, t5 f5) { \
		PROTOCOL_STRUCT(method) __entry = { f1, f2, f3, f4, f5 }; \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = &__entry; \
		int __size = sizeof (PROTOCOL_STRUCT(method)); \
		CLIENT_PROTOCOL_NAME (method) (f1, f2, f3, f4, f5);
#define BEGIN_PROTOCOL_ENTRY6(method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5,t6,f6) \
	void method (t1 f1, t2 f2, t3 f3, t4 f4, t5 f5, t6 f6) { \
		PROTOCOL_STRUCT(method) __entry = { f1, f2, f3, f4, f5, f6 }; \
		int __type = PROTOCOL_ID(method); \
		gpointer __data = &__entry; \
		int __size = sizeof (PROTOCOL_STRUCT(method)); \
		CLIENT_PROTOCOL_NAME (method) (f1, f2, f3, f4, f5, f6);

#define DEFAULT_PRINT()
#define CUSTOM_PRINT(_)

#define IS_ALWAYS_MATCH(_)
#define MATCH_INDEX(_)
#define IS_VTABLE_MATCH(_)

#define END_PROTOCOL_ENTRY \
		protocol_entry (__type, __data, __size); \
	}

#define END_PROTOCOL_ENTRY_FLUSH \
		protocol_entry (__type, __data, __size); \
		binary_protocol_flush_buffers (FALSE); \
	}

#ifdef SGEN_HEAVY_BINARY_PROTOCOL
#define BEGIN_PROTOCOL_ENTRY_HEAVY0(method) \
	BEGIN_PROTOCOL_ENTRY0 (method)
#define BEGIN_PROTOCOL_ENTRY_HEAVY1(method,t1,f1) \
	BEGIN_PROTOCOL_ENTRY1 (method,t1,f1)
#define BEGIN_PROTOCOL_ENTRY_HEAVY2(method,t1,f1,t2,f2) \
	BEGIN_PROTOCOL_ENTRY2 (method,t1,f1,t2,f2)
#define BEGIN_PROTOCOL_ENTRY_HEAVY3(method,t1,f1,t2,f2,t3,f3) \
	BEGIN_PROTOCOL_ENTRY3 (method,t1,f1,t2,f2,t3,f3)
#define BEGIN_PROTOCOL_ENTRY_HEAVY4(method,t1,f1,t2,f2,t3,f3,t4,f4) \
	BEGIN_PROTOCOL_ENTRY4 (method,t1,f1,t2,f2,t3,f3,t4,f4)
#define BEGIN_PROTOCOL_ENTRY_HEAVY5(method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5) \
	BEGIN_PROTOCOL_ENTRY5 (method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5)
#define BEGIN_PROTOCOL_ENTRY_HEAVY6(method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5,t6,f6) \
	BEGIN_PROTOCOL_ENTRY6 (method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5,t6,f6)

#define END_PROTOCOL_ENTRY_HEAVY \
	END_PROTOCOL_ENTRY
#else
#define BEGIN_PROTOCOL_ENTRY_HEAVY0(method)
#define BEGIN_PROTOCOL_ENTRY_HEAVY1(method,t1,f1)
#define BEGIN_PROTOCOL_ENTRY_HEAVY2(method,t1,f1,t2,f2)
#define BEGIN_PROTOCOL_ENTRY_HEAVY3(method,t1,f1,t2,f2,t3,f3)
#define BEGIN_PROTOCOL_ENTRY_HEAVY4(method,t1,f1,t2,f2,t3,f3,t4,f4)
#define BEGIN_PROTOCOL_ENTRY_HEAVY5(method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5)
#define BEGIN_PROTOCOL_ENTRY_HEAVY6(method,t1,f1,t2,f2,t3,f3,t4,f4,t5,f5,t6,f6)

#define END_PROTOCOL_ENTRY_HEAVY
#endif

#include "sgen-protocol-def.h"

#undef TYPE_INT
#undef TYPE_LONGLONG
#undef TYPE_SIZE
#undef TYPE_POINTER
#undef TYPE_BOOL

#endif /* HAVE_SGEN_GC */
