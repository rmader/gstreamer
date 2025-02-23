/* GStreamer udmabuf allocator
 *
 * Copyright (C) 2025 Collabora Ltd.
 * Author: Robert Mader <robert.mader@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gstudmabufallocator
 * @title: GstUdmabufAllocator
 * @short_description: Allocator for file-descriptor backed udmabuf
 * @see_also: #GstMemory and #GstFdAllocator
 *
 * This is a subclass of #GstFdAllocator that implements the
 * gst_allocator_alloc() method using `memfd_create()`. Platforms not supporting
 * that (Windows) will always return %NULL.
 *
 * Note that allocating new shared memories has a significant performance cost,
 * it is thus recommended to keep a pool of pre-allocated #GstMemory, using
 * #GstBufferPool. For that reason, this allocator has the
 * %GST_ALLOCATOR_FLAG_NO_COPY flag set.
 *
 * Since: 1.28
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstudmabufallocator.h"

#include <sys/stat.h>

#include "gst/gst_private.h"

#ifdef HAVE_MEMFD_CREATE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

struct _GstUdmabufAllocator
{
  GstDmaBufAllocator parent;

  GstMemoryCopyFunction fallback_copy;
  int udmabuf_dev_fd;
};

#define GST_CAT_DEFAULT gst_udmabuf_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define parent_class gst_udmabuf_allocator_parent_class
G_DEFINE_TYPE_WITH_CODE (GstUdmabufAllocator, gst_udmabuf_allocator,
    GST_TYPE_DMABUF_ALLOCATOR,
    GST_DEBUG_CATEGORY_INIT (gst_udmabuf_debug, "udmabufallocator", 0,
        "udmabuf allocator");
    );

#define UDMABUF_CREATE        _IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_FLAGS_CLOEXEC 0x01

// typedef guint32 __u32;
// typedef guint64 __u64;

struct udmabuf_create
{
  guint32 memfd;
  guint32 flags;
  guint64 offset;
  guint64 size;
};

static GstMemory *
gst_udmabuf_allocator_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
#ifdef HAVE_MEMFD_CREATE
  GstUdmabufAllocator *self = GST_UDMABUF_ALLOCATOR (allocator);
  struct udmabuf_create create;
  int fd, ufd;
  GstMemory *mem;
  GstMapInfo info;
  gsize maxsize;

  maxsize = size + params->prefix + params->padding;
  if (!g_size_checked_add (&maxsize, maxsize, params->align)) {
    GST_ERROR_OBJECT (self, "Requested buffer size too big");
    return NULL;
  }
  maxsize &= ~params->align;

#ifdef HAVE_GETPAGESIZE
  gsize pagesizemask = getpagesize () - 1;
  if (!g_size_checked_add (&maxsize, maxsize, pagesizemask)) {
    GST_ERROR_OBJECT (self, "Requested buffer size too big");
    return NULL;
  }
  maxsize &= ~pagesizemask;
#endif

  fd = memfd_create ("gst-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    GST_ERROR_OBJECT (self, "memfd_create() failed: %s", strerror (errno));
    return NULL;
  }

  if (ftruncate (fd, maxsize) < 0) {
    GST_ERROR_OBJECT (self, "ftruncate failed: %s", strerror (errno));
    close (fd);
    return NULL;
  }

  if (fcntl (fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
    GST_ERROR_OBJECT (self, "adding seals failed: %s", strerror (errno));
    close (fd);
    return NULL;
  }

  create.memfd = fd;
  create.flags = UDMABUF_FLAGS_CLOEXEC;
  create.offset = 0;
  create.size = maxsize;

  ufd = ioctl (self->udmabuf_dev_fd, UDMABUF_CREATE, &create);
  if (ufd < 0) {
    GST_ERROR_OBJECT (self, "creating udmabuf failed: %s", strerror (errno));
    close (fd);
    return NULL;
  }
  /* The underlying memfd is kept as as a reference in the kernel. */
  close (fd);

  mem = gst_dmabuf_allocator_alloc_with_flags (allocator, ufd, maxsize,
      GST_FD_MEMORY_FLAG_KEEP_MAPPED);
  if (G_UNLIKELY (!mem)) {
    GST_ERROR_OBJECT (self, "allocation failed");
    close (ufd);
    return NULL;
  }

  /* We use GST_FD_MEMORY_FLAG_KEEP_MAPPED, so make sure the first map is RW. */
  if (!gst_memory_map (mem, &info, GST_MAP_READWRITE)) {
    GST_ERROR_OBJECT (self, "map failed");
    gst_memory_unref (mem);
    return NULL;
  }
  gst_memory_unmap (mem, &info);

  mem->align = params->align;
  mem->maxsize = maxsize;
  mem->offset = params->prefix;
  mem->size = size;

  return mem;
#else
  return NULL;
#endif
}

static GstMemory *
gst_udmabuf_mem_copy (GstMemory * mem, gssize offset, gsize size)
{
  GstUdmabufAllocator *self = GST_UDMABUF_ALLOCATOR (mem->allocator);
  GstAllocationParams params = { 0 };
  GstMemory *copy;
  GstMapInfo info_mem;
  GstMapInfo info_copy;

  /* non-zero offset or different size is not supported */
  if (offset != 0 || (size != -1 && (gsize) size != mem->size)) {
    GST_DEBUG_OBJECT (self, "Different size/offset, try fallback copy");
    return self->fallback_copy (mem, offset, size);
  }

  if (!gst_memory_map (mem, &info_mem, GST_MAP_READ)) {
    GST_WARNING_OBJECT (self, "Failed to map memory, try fallback copy");
    return self->fallback_copy (mem, offset, size);
  }

  params.align = mem->align;
  params.prefix = mem->offset;
  params.padding = mem->maxsize - (mem->offset + mem->size);

  copy =
      gst_udmabuf_allocator_alloc (GST_ALLOCATOR_CAST (self), mem->size,
      &params);
  if (!copy) {
    GST_WARNING_OBJECT (self, "Failed to allocate memory, try fallback copy");
    gst_memory_unmap (mem, &info_mem);
    return self->fallback_copy (mem, offset, size);
  }

  if (!gst_memory_map (copy, &info_copy, GST_MAP_WRITE)) {
    GST_WARNING_OBJECT (self,
        "Failed to map memory of copied buffer, try fallback copy");
    gst_memory_unmap (mem, &info_mem);
    gst_memory_unref (copy);
    return self->fallback_copy (mem, offset, size);
  }

  memcpy (info_copy.data, info_mem.data, mem->size);

  gst_memory_unmap (mem, &info_mem);
  gst_memory_unmap (copy, &info_copy);

  GST_CAT_DEBUG (GST_CAT_PERFORMANCE,
      "memcpy %" G_GSIZE_FORMAT " memory %p -> %p", mem->size, mem, copy);
  GST_DEBUG_OBJECT (self, "memcpy %" G_GSIZE_FORMAT " memory %p -> %p",
      mem->size, mem, copy);

  return copy;
}

static void
gst_udmabuf_allocator_finalize (GObject * obj)
{
  GstUdmabufAllocator *self = GST_UDMABUF_ALLOCATOR (obj);

  if (self->udmabuf_dev_fd != -1) {
    close (self->udmabuf_dev_fd);
    self->udmabuf_dev_fd = -1;
  }

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_udmabuf_allocator_class_init (GstUdmabufAllocatorClass * klass)
{
  GstAllocatorClass *alloc_class = (GstAllocatorClass *) klass;
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  alloc_class->alloc = GST_DEBUG_FUNCPTR (gst_udmabuf_allocator_alloc);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_udmabuf_allocator_finalize);
}

static void
gst_udmabuf_allocator_init (GstUdmabufAllocator * self)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (self);

  GST_OBJECT_FLAG_SET (self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);

  alloc->mem_type = GST_ALLOCATOR_UDMABUF;
  self->fallback_copy = alloc->mem_copy;
  alloc->mem_copy = (GstMemoryCopyFunction) gst_udmabuf_mem_copy;

  self->udmabuf_dev_fd = open ("/dev/udmabuf", O_RDWR | O_CLOEXEC, 0);
  if (self->udmabuf_dev_fd == -1)
    GST_WARNING
        ("Udmabuf allocator not available, can't open /dev/udmabuf: %s",
        strerror (errno));
}

/**
 * gst_udmabuf_allocator_init_once:
 *
 * Register a #GstUdmabufAllocator using gst_allocator_register() with the name
 * %GST_ALLOCATOR_UDMABUF. This is no-op after the first call.
 *
 * Since: 1.28
 */
void
gst_udmabuf_allocator_init_once (void)
{
#if defined(HAVE_MEMFD_CREATE)
  static gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GstUdmabufAllocator *alloc;

    alloc =
        (GstUdmabufAllocator *) g_object_new (GST_TYPE_UDMABUF_ALLOCATOR, NULL);
    gst_object_ref_sink (alloc);

    if (alloc->udmabuf_dev_fd != -1)
      gst_allocator_register (GST_ALLOCATOR_UDMABUF, GST_ALLOCATOR (alloc));
    else
      gst_object_unref (alloc);

    g_once_init_leave (&_init, 1);
  }
#endif
}
