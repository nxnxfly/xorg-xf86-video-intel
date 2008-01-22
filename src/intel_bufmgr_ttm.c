/**************************************************************************
 * 
 * Copyright � 2007 Red Hat Inc.
 * Copyright � 2007 Intel Corporation
 * Copyright 2006 Tungsten Graphics, Inc., Bismarck, ND., USA
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE 
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * 
 **************************************************************************/
/*
 * Authors: Thomas Hellstr�m <thomas-at-tungstengraphics-dot-com>
 *          Keith Whitwell <keithw-at-tungstengraphics-dot-com>
 *	    Eric Anholt <eric@anholt.net>
 *	    Dave Airlie <airlied@linux.ie>
 */

#include <xf86drm.h>
#include <stdlib.h>
#include <unistd.h>
#include "errno.h"
#include "dri_bufmgr.h"
#include <xf86mm.h>

#include "i915_drm.h"

#include "intel_bufmgr_ttm.h"

#define DBG(...) do {					\
   if (bufmgr_ttm->bufmgr.debug)			\
     ErrorF(__VA_ARGS__);			\
} while (0)

/*
 * These bits are always specified in each validation
 * request. Other bits are not supported at this point
 * as it would require a bit of investigation to figure
 * out what mask value should be used.
 */
#define INTEL_BO_MASK  (DRM_BO_MASK_MEM | \
			DRM_BO_FLAG_READ | \
			DRM_BO_FLAG_WRITE | \
			DRM_BO_FLAG_EXE)

struct intel_validate_entry {
    ddx_bo *bo;
    struct drm_i915_op_arg bo_arg;
};

typedef struct _ddx_bufmgr_ttm {
   ddx_bufmgr bufmgr;

   int fd;
   unsigned int fence_type;
   unsigned int fence_type_flush;

   uint32_t max_relocs;

    struct intel_validate_entry *validate_array;
    int validate_array_size;
    int validate_count;

    drmBO *cached_reloc_buf;
    uint32_t *cached_reloc_buf_data;
} ddx_bufmgr_ttm;


/**
 * Private information associated with a relocation that isn't already stored
 * in the relocation buffer to be passed to the kernel.
 */
struct _ddx_ttm_reloc {
    ddx_bo *target_buf;
    uint64_t validate_flags;
};

typedef struct _ddx_bo_ttm {
   ddx_bo bo;

   int refcount;		/* Protected by bufmgr->mutex */
   drmBO drm_bo;
   const char *name;

    uint64_t last_flags;

    /**
     * Index of the buffer within the validation list while preparing a
     * batchbuffer execution.
     */
    int validate_index;

    /** DRM buffer object containing relocation list */
    drmBO *reloc_buf;
    uint32_t *reloc_buf_data;
    struct _ddx_ttm_reloc *relocs;

    /**
     * Indicates that the buffer may be shared with other processes, so we
     * can't hold maps beyond when the user does.
     */
    Bool shared;

    Bool delayed_unmap;
    /* Virtual address from the dri_bo_map whose unmap was delayed. */
    void *saved_virtual;
} ddx_bo_ttm;

typedef struct _dri_fence_ttm
{
   dri_fence fence;

   int refcount;		/* Protected by bufmgr->mutex */
   const char *name;
   drmFence drm_fence;
} dri_fence_ttm;

static void dri_ttm_dump_validation_list(ddx_bufmgr_ttm *bufmgr_ttm)
{
    int i, j;

    for (i = 0; i < bufmgr_ttm->validate_count; i++) {
	ddx_bo *bo = bufmgr_ttm->validate_array[i].bo;
	ddx_bo_ttm *bo_ttm = (ddx_bo_ttm *)bo;

	if (bo_ttm->reloc_buf_data != NULL) {
	    for (j = 0; j < (bo_ttm->reloc_buf_data[0] & 0xffff); j++) {
		uint32_t *reloc_entry = bo_ttm->reloc_buf_data +
		    I915_RELOC_HEADER +
		    j * I915_RELOC0_STRIDE;
		ddx_bo *target_bo =
		    bufmgr_ttm->validate_array[reloc_entry[2]].bo;
		ddx_bo_ttm *target_ttm = (ddx_bo_ttm *)target_bo;

		DBG("%2d: %s@0x%08x -> %s@0x%08x + 0x%08x\n",
		    i,
		    bo_ttm->name, reloc_entry[0],
		    target_ttm->name, target_bo->offset,
		    reloc_entry[1]);
	    }
	} else {
	    DBG("%2d: %s\n", i, bo_ttm->name);
	}
    }
}

/**
 * Adds the given buffer to the list of buffers to be validated (moved into the
 * appropriate memory type) with the next batch submission.
 *
 * If a buffer is validated multiple times in a batch submission, it ends up
 * with the intersection of the memory type flags and the union of the
 * access flags.
 */
static void
intelddx_add_validate_buffer(ddx_bo *buf,
			  uint64_t flags)
{
    ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)buf->bufmgr;
    ddx_bo_ttm *ttm_buf = (ddx_bo_ttm *)buf;

    /* If we delayed doing an unmap to mitigate map/unmap syscall thrashing,
     * do that now.
     */
    if (ttm_buf->delayed_unmap) {
	drmBOUnmap(bufmgr_ttm->fd, &ttm_buf->drm_bo);
	ttm_buf->delayed_unmap = FALSE;
    }

    if (ttm_buf->validate_index == -1) {
	struct intel_validate_entry *entry;
	struct drm_i915_op_arg *arg;
	struct drm_bo_op_req *req;
	int index;

	/* Extend the array of validation entries as necessary. */
	if (bufmgr_ttm->validate_count == bufmgr_ttm->validate_array_size) {
	    int i, new_size = bufmgr_ttm->validate_array_size * 2;

	    if (new_size == 0)
		new_size = 5;

	    bufmgr_ttm->validate_array =
	       realloc(bufmgr_ttm->validate_array,
		       sizeof(struct intel_validate_entry) * new_size);
	    bufmgr_ttm->validate_array_size = new_size;

	    /* Update pointers for realloced mem. */
	    for (i = 0; i < bufmgr_ttm->validate_count - 1; i++) {
	       bufmgr_ttm->validate_array[i].bo_arg.next = (unsigned long)
		  &bufmgr_ttm->validate_array[i + 1].bo_arg;
	    }
	}

	/* Pick out the new array entry for ourselves */
	index = bufmgr_ttm->validate_count;
	ttm_buf->validate_index = index;
	entry = &bufmgr_ttm->validate_array[index];
	bufmgr_ttm->validate_count++;

	/* Fill in array entry */
	entry->bo = buf;
	ddx_bo_reference(buf);

	/* Fill in kernel arg */
	arg = &entry->bo_arg;
	req = &arg->d.req;

	memset(arg, 0, sizeof(*arg));
	req->bo_req.handle = ttm_buf->drm_bo.handle;
	req->op = drm_bo_validate;
	req->bo_req.flags = flags;
	req->bo_req.hint = 0;
#ifdef DRM_BO_HINT_PRESUMED_OFFSET
	req->bo_req.hint |= DRM_BO_HINT_PRESUMED_OFFSET;
	req->bo_req.presumed_offset = buf->offset;
#endif
	req->bo_req.mask = INTEL_BO_MASK;
	req->bo_req.fence_class = 0; /* Backwards compat. */

	if (ttm_buf->reloc_buf != NULL)
	    arg->reloc_handle = ttm_buf->reloc_buf->handle;
	else
	    arg->reloc_handle = 0;

	/* Hook up the linked list of args for the kernel */
	arg->next = 0;
	if (index != 0) {
	    bufmgr_ttm->validate_array[index - 1].bo_arg.next =
		(unsigned long)arg;
	}
    } else {
	struct intel_validate_entry *entry =
	    &bufmgr_ttm->validate_array[ttm_buf->validate_index];
	struct drm_i915_op_arg *arg = &entry->bo_arg;
	struct drm_bo_op_req *req = &arg->d.req;
	uint64_t memFlags = req->bo_req.flags & flags & DRM_BO_MASK_MEM;
	uint64_t modeFlags = (req->bo_req.flags | flags) & ~DRM_BO_MASK_MEM;

	/* Buffer was already in the validate list.  Extend its flags as
	 * necessary.
	 */

	if (memFlags == 0) {
	    fprintf(stderr,
		    "%s: No shared memory types between "
		    "0x%16llx and 0x%16llx\n",
		    __FUNCTION__, req->bo_req.flags, flags);
	    abort();
	}
	if (flags & ~INTEL_BO_MASK) {
	    fprintf(stderr,
		    "%s: Flags bits 0x%16llx are not supposed to be used in a relocation\n",
		    __FUNCTION__, flags & ~INTEL_BO_MASK);
	    abort();
	}
	req->bo_req.flags = memFlags | modeFlags;
    }
}

#define RELOC_BUF_SIZE(x) ((I915_RELOC_HEADER + x * I915_RELOC0_STRIDE) * \
	sizeof(uint32_t))

static int
intelddx_setup_reloc_list(ddx_bo *bo)
{
    ddx_bo_ttm *bo_ttm = (ddx_bo_ttm *)bo;
    ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)bo->bufmgr;
    int ret;

    bo_ttm->relocs = malloc(sizeof(struct _ddx_ttm_reloc) *
			    bufmgr_ttm->max_relocs);

    if (bufmgr_ttm->cached_reloc_buf != NULL) {
       bo_ttm->reloc_buf = bufmgr_ttm->cached_reloc_buf;
       bo_ttm->reloc_buf_data = bufmgr_ttm->cached_reloc_buf_data;

       bufmgr_ttm->cached_reloc_buf = NULL;
       bufmgr_ttm->cached_reloc_buf_data = NULL;
    } else {
       bo_ttm->reloc_buf = malloc(sizeof(bo_ttm->drm_bo));
       ret = drmBOCreate(bufmgr_ttm->fd,
			 RELOC_BUF_SIZE(bufmgr_ttm->max_relocs), 0,
			 NULL,
			 DRM_BO_FLAG_MEM_LOCAL |
			 DRM_BO_FLAG_READ |
			 DRM_BO_FLAG_WRITE |
			 DRM_BO_FLAG_MAPPABLE |
			 DRM_BO_FLAG_CACHED,
			 0, bo_ttm->reloc_buf);
       if (ret) {
	  fprintf(stderr, "Failed to create relocation BO: %s\n",
		  strerror(-ret));
	  return ret;
       }

       ret = drmBOMap(bufmgr_ttm->fd, bo_ttm->reloc_buf,
		      DRM_BO_FLAG_READ | DRM_BO_FLAG_WRITE,
		      0, (void **)&bo_ttm->reloc_buf_data);
       if (ret) {
	  fprintf(stderr, "Failed to map relocation BO: %s\n",
		  strerror(-ret));
	  return ret;
       }
    }

    /* Initialize the relocation list with the header:
     * DWORD 0: relocation type, relocation count
     * DWORD 1: handle to next relocation list (currently none)
     * DWORD 2: unused
     * DWORD 3: unused
     */
    bo_ttm->reloc_buf_data[0] = I915_RELOC_TYPE_0 << 16;
    bo_ttm->reloc_buf_data[1] = 0;
    bo_ttm->reloc_buf_data[2] = 0;
    bo_ttm->reloc_buf_data[3] = 0;

    return 0;
}

#if 0
int
driFenceSignaled(DriFenceObject * fence, unsigned type)
{
   int signaled;
   int ret;

   if (fence == NULL)
      return TRUE;

   ret = drmFenceSignaled(bufmgr_ttm->fd, &fence->fence, type, &signaled);
   BM_CKFATAL(ret);
   return signaled;
}
#endif

static ddx_bo *
dri_ttm_alloc(ddx_bufmgr *bufmgr, const char *name,
	      unsigned long size, unsigned int alignment,
	      uint64_t  location_mask)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)bufmgr;
   ddx_bo_ttm *ttm_buf;
   unsigned int pageSize = getpagesize();
   int ret;
   unsigned int flags, hint;

   ttm_buf = malloc(sizeof(*ttm_buf));
   if (!ttm_buf)
      return NULL;

   /* The mask argument doesn't do anything for us that we want other than
    * determine which pool (TTM or local) the buffer is allocated into, so just
    * pass all of the allocation class flags.
    */
   flags = location_mask | DRM_BO_FLAG_READ | DRM_BO_FLAG_WRITE |
      DRM_BO_FLAG_EXE;
   /* No hints we want to use. */
   hint = 0;

   ret = drmBOCreate(bufmgr_ttm->fd, size, alignment / pageSize,
		     NULL, flags, hint, &ttm_buf->drm_bo);
   if (ret != 0) {
      free(ttm_buf);
      return NULL;
   }
   ttm_buf->bo.size = ttm_buf->drm_bo.size;
   ttm_buf->bo.offset = ttm_buf->drm_bo.offset;
   ttm_buf->bo.virtual = NULL;
   ttm_buf->bo.bufmgr = bufmgr;
   ttm_buf->name = name;
   ttm_buf->refcount = 1;
   ttm_buf->reloc_buf = NULL;
   ttm_buf->reloc_buf_data = NULL;
   ttm_buf->relocs = NULL;
   ttm_buf->last_flags = ttm_buf->drm_bo.flags;
   ttm_buf->shared = FALSE;
   ttm_buf->delayed_unmap = FALSE;
   ttm_buf->validate_index = -1;

#if BUFMGR_DEBUG
   fprintf(stderr, "bo_create: %p (%s)\n", &ttm_buf->bo, ttm_buf->name);
#endif

   return &ttm_buf->bo;
}

/* Our TTM backend doesn't allow creation of static buffers, as that requires
 * privelege for the non-fake case, and the lock in the fake case where we were
 * working around the X Server not creating buffers and passing handles to us.
 */
static ddx_bo *
dri_ttm_alloc_static(ddx_bufmgr *bufmgr, const char *name,
		     unsigned long offset, unsigned long size, void *virtual,
		     uint64_t location_mask)
{
   return NULL;
}

/** Returns a ddx_bo wrapping the given buffer object handle.
 *
 * This can be used when one application needs to pass a buffer object
 * to another.
 */
ddx_bo *
intelddx_ttm_bo_create_from_handle(ddx_bufmgr *bufmgr, const char *name,
			      unsigned int handle)
{
   ddx_bufmgr_ttm *bufmgr_ttm;
   ddx_bo_ttm *ttm_buf;
   int ret;

   bufmgr_ttm = (ddx_bufmgr_ttm *)bufmgr;

   ttm_buf = malloc(sizeof(*ttm_buf));
   if (!ttm_buf)
      return NULL;

   ret = drmBOReference(bufmgr_ttm->fd, handle, &ttm_buf->drm_bo);
   if (ret != 0) {
      free(ttm_buf);
      return NULL;
   }
   ttm_buf->bo.size = ttm_buf->drm_bo.size;
   ttm_buf->bo.offset = ttm_buf->drm_bo.offset;
   ttm_buf->bo.virtual = NULL;
   ttm_buf->bo.bufmgr = bufmgr;
   ttm_buf->name = name;
   ttm_buf->refcount = 1;
   ttm_buf->reloc_buf = NULL;
   ttm_buf->reloc_buf_data = NULL;
   ttm_buf->relocs = NULL;
   ttm_buf->last_flags = ttm_buf->drm_bo.flags;
   ttm_buf->shared = TRUE;
   ttm_buf->delayed_unmap = FALSE;
   ttm_buf->validate_index = -1;

#if BUFMGR_DEBUG
   fprintf(stderr, "bo_create_from_handle: %p %08x (%s)\n", &ttm_buf->bo, handle,
	   ttm_buf->name);
#endif

   return &ttm_buf->bo;
}

static void
dri_ttm_bo_reference(ddx_bo *buf)
{
   ddx_bo_ttm *ttm_buf = (ddx_bo_ttm *)buf;

   ttm_buf->refcount++;
}

static void
dri_ttm_bo_unreference(ddx_bo *buf)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)buf->bufmgr;
   ddx_bo_ttm *ttm_buf = (ddx_bo_ttm *)buf;

   if (!buf)
      return;

   if (--ttm_buf->refcount == 0) {
      int ret;

      	if (ttm_buf->reloc_buf) {
	    int i;

	    /* Unreference all the target buffers */
	    for (i = 0; i < (ttm_buf->reloc_buf_data[0] & 0xffff); i++)
		 ddx_bo_unreference(ttm_buf->relocs[i].target_buf);
	    free(ttm_buf->relocs);

	    if (bufmgr_ttm->cached_reloc_buf == NULL) {
	       /* Cache a single relocation buffer allocation to avoid
		* repeated create/map/unmap/destroy for batchbuffer
		* relocations.
		*/
	       bufmgr_ttm->cached_reloc_buf = ttm_buf->reloc_buf;
	       bufmgr_ttm->cached_reloc_buf_data = ttm_buf->reloc_buf_data;
	    } else {
	       /* Free the kernel BO containing relocation entries */
	       drmBOUnmap(bufmgr_ttm->fd, ttm_buf->reloc_buf);
	       drmBOUnreference(bufmgr_ttm->fd, ttm_buf->reloc_buf);
	       free(ttm_buf->reloc_buf);
	    }
	}

	if (ttm_buf->delayed_unmap)
	   drmBOUnmap(bufmgr_ttm->fd, &ttm_buf->drm_bo);

	ret = drmBOUnreference(bufmgr_ttm->fd, &ttm_buf->drm_bo);
	if (ret != 0) {
	  fprintf(stderr, "drmBOUnreference failed (%s): %s\n", ttm_buf->name,
		  strerror(-ret));
	}
#if BUFMGR_DEBUG
	fprintf(stderr, "bo_unreference final: %p (%s)\n",
		&ttm_buf->bo, ttm_buf->name);
#endif
	free(buf);
	return;
   }
}

static int
dri_ttm_bo_map(ddx_bo *buf, Bool write_enable)
{
   ddx_bufmgr_ttm *bufmgr_ttm;
   ddx_bo_ttm *ttm_buf = (ddx_bo_ttm *)buf;
   unsigned int flags;

   bufmgr_ttm = (ddx_bufmgr_ttm *)buf->bufmgr;

   flags = DRM_BO_FLAG_READ;
   if (write_enable)
       flags |= DRM_BO_FLAG_WRITE;

   assert(buf->virtual == NULL);

    DBG("bo_map: %p (%s)\n", &ttm_buf->bo, ttm_buf->name);

    /* XXX: What about if we're upgrading from READ to WRITE? */
    if (ttm_buf->delayed_unmap) {
	buf->virtual = ttm_buf->saved_virtual;
	return 0;
    }

   return drmBOMap(bufmgr_ttm->fd, &ttm_buf->drm_bo, flags, 0, &buf->virtual);
}

static int
dri_ttm_bo_unmap(ddx_bo *buf)
{
   ddx_bufmgr_ttm *bufmgr_ttm;
   ddx_bo_ttm *ttm_buf = (ddx_bo_ttm *)buf;

   if (buf == NULL)
      return 0;

   bufmgr_ttm = (ddx_bufmgr_ttm *)buf->bufmgr;

   assert(buf->virtual != NULL);

   if (!ttm_buf->shared) {
	ttm_buf->saved_virtual = buf->virtual;
	ttm_buf->delayed_unmap = TRUE;
	buf->virtual = NULL;
	return 0;
   }
   buf->virtual = NULL;


#if BUFMGR_DEBUG
   fprintf(stderr, "bo_unmap: %p (%s)\n", &ttm_buf->bo, ttm_buf->name);
#endif

   return drmBOUnmap(bufmgr_ttm->fd, &ttm_buf->drm_bo);
}

/* Returns a ddx_bo wrapping the given buffer object handle.
 *
 * This can be used when one application needs to pass a buffer object
 * to another.
 */
dri_fence *
intelddx_ttm_fence_create_from_arg(ddx_bufmgr *bufmgr, const char *name,
				drm_fence_arg_t *arg)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)bufmgr;
   dri_fence_ttm *ttm_fence;

    ttm_fence = malloc(sizeof(*ttm_fence));
   if (!ttm_fence)
      return NULL;

   ttm_fence->drm_fence.handle = arg->handle;
   ttm_fence->drm_fence.fence_class = arg->fence_class;
   ttm_fence->drm_fence.type = arg->type;
   ttm_fence->drm_fence.flags = arg->flags;
   ttm_fence->drm_fence.signaled = 0;
   ttm_fence->drm_fence.sequence = arg->sequence;

   ttm_fence->fence.bufmgr = bufmgr;
   ttm_fence->name = name;
   ttm_fence->refcount = 1;

#if BUFMGR_DEBUG
   fprintf(stderr, "fence_create_from_handle: %p (%s)\n", &ttm_fence->fence,
	   ttm_fence->name);
#endif

   return &ttm_fence->fence;
}


static void
dri_ttm_fence_reference(dri_fence *fence)
{
   dri_fence_ttm *fence_ttm = (dri_fence_ttm *)fence;

   ++fence_ttm->refcount;
#if BUFMGR_DEBUG
   fprintf(stderr, "fence_reference: %p (%s)\n", &fence_ttm->fence,
	   fence_ttm->name);
#endif
}

static void
dri_ttm_fence_unreference(dri_fence *fence)
{
   dri_fence_ttm *fence_ttm = (dri_fence_ttm *)fence;
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)fence->bufmgr;

   if (!fence)
      return;

#if BUFMGR_DEBUG
   fprintf(stderr, "fence_unreference: %d %p (%s)\n", fence_ttm->refcount, &fence_ttm->fence,
	   fence_ttm->name);
#endif
   if (--fence_ttm->refcount == 0) {
      int ret;

      ret = drmFenceUnreference(bufmgr_ttm->fd, &fence_ttm->drm_fence);
      if (ret != 0) {
	 fprintf(stderr, "drmFenceUnreference failed (%s): %s\n",
		 fence_ttm->name, strerror(-ret));
      }

      free(fence);
      return;
   }
}

static void
dri_ttm_fence_wait(dri_fence *fence)
{
   dri_fence_ttm *fence_ttm = (dri_fence_ttm *)fence;
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)fence->bufmgr;
   int ret;

   ret = drmFenceWait(bufmgr_ttm->fd, DRM_FENCE_FLAG_WAIT_LAZY, &fence_ttm->drm_fence, 0);
   if (ret != 0) {
      ErrorF("%s:%d: Error %d waiting for fence %s.\n",
		   __FILE__, __LINE__, ret, fence_ttm->name);
      abort();
   }

#if BUFMGR_DEBUG
   fprintf(stderr, "fence_wait: %p (%s)\n", &fence_ttm->fence,
	   fence_ttm->name);
#endif
}

static void
ddx_bufmgr_ttm_destroy(ddx_bufmgr *bufmgr)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)bufmgr;

    if (bufmgr_ttm->cached_reloc_buf) {
       /* Free the cached kernel BO containing relocation entries */
       drmBOUnmap(bufmgr_ttm->fd, bufmgr_ttm->cached_reloc_buf);
       drmBOUnreference(bufmgr_ttm->fd, bufmgr_ttm->cached_reloc_buf);
       free(bufmgr_ttm->cached_reloc_buf);
    }

    free(bufmgr_ttm->validate_array);

   free(bufmgr);
}

/**
 * Adds the target buffer to the validation list and adds the relocation
 * to the reloc_buffer's relocation list.
 *
 * The relocation entry at the given offset must already contain the
 * precomputed relocation value, because the kernel will optimize out
 * the relocation entry write when the buffer hasn't moved from the
 * last known offset in target_buf.
 */
static void
dri_ttm_emit_reloc(ddx_bo *reloc_buf, uint64_t flags, uint32_t delta,
		   uint32_t offset, ddx_bo *target_buf)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)reloc_buf->bufmgr;
   ddx_bo_ttm *reloc_buf_ttm = (ddx_bo_ttm *)reloc_buf;
   int num_relocs;
   uint32_t *this_reloc;

    /* Create a new relocation list if needed */
    if (reloc_buf_ttm->reloc_buf == NULL)
	intelddx_setup_reloc_list(reloc_buf);

    num_relocs = (reloc_buf_ttm->reloc_buf_data[0] & 0xffff);

    /* Check overflow */
    assert((reloc_buf_ttm->reloc_buf_data[0] & 0xffff) <
	   bufmgr_ttm->max_relocs);

    this_reloc = reloc_buf_ttm->reloc_buf_data + I915_RELOC_HEADER +
	num_relocs * I915_RELOC0_STRIDE;

    this_reloc[0] = offset;
    this_reloc[1] = delta;
    this_reloc[2] = -1; /* To be filled in at exec time */
    this_reloc[3] = 0;

    reloc_buf_ttm->relocs[num_relocs].validate_flags = flags;
    reloc_buf_ttm->relocs[num_relocs].target_buf = target_buf;
    ddx_bo_reference(target_buf);

    reloc_buf_ttm->reloc_buf_data[0]++; /* Increment relocation count */
    /* Check wraparound */
    assert((reloc_buf_ttm->reloc_buf_data[0] & 0xffff) != 0);
   return;
}

/**
 * Walk the tree of relocations rooted at BO and accumulate the list of
 * validations to be performed and update the relocation buffers with
 * index values into the validation list.
 */
static void
dri_ttm_bo_process_reloc(ddx_bo *bo)
{
    ddx_bo_ttm *bo_ttm = (ddx_bo_ttm *)bo;
    unsigned int nr_relocs;
    int i;

    if (bo_ttm->reloc_buf_data == NULL)
	return;

    nr_relocs = bo_ttm->reloc_buf_data[0] & 0xffff;

    for (i = 0; i < nr_relocs; i++) {
	struct _ddx_ttm_reloc *r = &bo_ttm->relocs[i];
	ddx_bo_ttm *target_ttm = (ddx_bo_ttm *)r->target_buf;
	uint32_t *reloc_entry;

	/* Continue walking the tree depth-first. */
	dri_ttm_bo_process_reloc(r->target_buf);

	/* Add the target to the validate list */
	intelddx_add_validate_buffer(r->target_buf, r->validate_flags);

	/* Update the index of the target in the relocation entry */
	reloc_entry = bo_ttm->reloc_buf_data + I915_RELOC_HEADER +
	    i * I915_RELOC0_STRIDE;
	reloc_entry[2] = target_ttm->validate_index;
    }
}

static void *
dri_ttm_process_reloc(ddx_bo *batch_buf, uint32_t *count)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)batch_buf->bufmgr;

   /* Update indices and set up the validate list. */
   dri_ttm_bo_process_reloc(batch_buf);
   
   /* Add the batch buffer to the validation list.  There are no relocations
    * pointing to it.
    */
    intelddx_add_validate_buffer(batch_buf,
			      DRM_BO_FLAG_MEM_TT | DRM_BO_FLAG_EXE);
    
    *count = bufmgr_ttm->validate_count;
    return &bufmgr_ttm->validate_array[0].bo_arg;
}

static const char *
intel_get_flags_mem_type_string(uint64_t flags)
{
    switch (flags & DRM_BO_MASK_MEM) {
    case DRM_BO_FLAG_MEM_LOCAL: return "local";
    case DRM_BO_FLAG_MEM_TT: return "ttm";
    case DRM_BO_FLAG_MEM_VRAM: return "vram";
    case DRM_BO_FLAG_MEM_PRIV0: return "priv0";
    case DRM_BO_FLAG_MEM_PRIV1: return "priv1";
    case DRM_BO_FLAG_MEM_PRIV2: return "priv2";
    case DRM_BO_FLAG_MEM_PRIV3: return "priv3";
    case DRM_BO_FLAG_MEM_PRIV4: return "priv4";
    default: return NULL;
    }
}

static const char *
intel_get_flags_caching_string(uint64_t flags)
{
    switch (flags & (DRM_BO_FLAG_CACHED | DRM_BO_FLAG_CACHED_MAPPED)) {
    case 0: return "UU";
    case DRM_BO_FLAG_CACHED: return "CU";
    case DRM_BO_FLAG_CACHED_MAPPED: return "UC";
    case DRM_BO_FLAG_CACHED | DRM_BO_FLAG_CACHED_MAPPED: return "CC";
    default: return NULL;
    }
}

static void
intel_update_buffer_offsets (ddx_bufmgr_ttm *bufmgr_ttm)
{
    int i;

    for (i = 0; i < bufmgr_ttm->validate_count; i++) {
	ddx_bo *bo = bufmgr_ttm->validate_array[i].bo;
	ddx_bo_ttm *bo_ttm = (ddx_bo_ttm *)bo;
	struct drm_i915_op_arg *arg = &bufmgr_ttm->validate_array[i].bo_arg;
	struct drm_bo_arg_rep *rep = &arg->d.rep;

	/* Update the flags */
	if (rep->bo_info.flags != bo_ttm->last_flags) {
	    DBG("BO %s migrated: %s/%s -> %s/%s\n",
		bo_ttm->name,
		intel_get_flags_mem_type_string(bo_ttm->last_flags),
		intel_get_flags_caching_string(bo_ttm->last_flags),
		intel_get_flags_mem_type_string(rep->bo_info.flags),
		intel_get_flags_caching_string(rep->bo_info.flags));

	    bo_ttm->last_flags = rep->bo_info.flags;
	}
	/* Update the buffer offset */
	if (rep->bo_info.offset != bo->offset) {
	    DBG("BO %s migrated: 0x%08x -> 0x%08x\n",
		bo_ttm->name, bo->offset, rep->bo_info.offset);
	    bo->offset = rep->bo_info.offset;
	}
    }
}

static void
dri_ttm_post_submit(ddx_bo *batch_buf, dri_fence **last_fence)
{
   ddx_bufmgr_ttm *bufmgr_ttm = (ddx_bufmgr_ttm *)batch_buf->bufmgr;
   int i;
    
   intel_update_buffer_offsets (bufmgr_ttm);

   if (bufmgr_ttm->bufmgr.debug)
     dri_ttm_dump_validation_list(bufmgr_ttm);
   
   for (i = 0; i < bufmgr_ttm->validate_count; i++) {
     ddx_bo *bo = bufmgr_ttm->validate_array[i].bo;
     ddx_bo_ttm *bo_ttm = (ddx_bo_ttm *)bo;
     
	/* Disconnect the buffer from the validate list */
     bo_ttm->validate_index = -1;
     ddx_bo_unreference(bo);
     bufmgr_ttm->validate_array[i].bo = NULL;
   }
   bufmgr_ttm->validate_count = 0;

}

/**
 * Initializes the TTM buffer manager, which uses the kernel to allocate, map,
 * and manage map buffer objections.
 *
 * \param fd File descriptor of the opened DRM device.
 * \param fence_type Driver-specific fence type used for fences with no flush.
 * \param fence_type_flush Driver-specific fence type used for fences with a
 *	  flush.
 */
ddx_bufmgr *
intelddx_bufmgr_ttm_init(int fd, unsigned int fence_type,
		      unsigned int fence_type_flush, int batch_size)
{
   ddx_bufmgr_ttm *bufmgr_ttm;

   bufmgr_ttm = calloc(1, sizeof(*bufmgr_ttm));
   bufmgr_ttm->fd = fd;
   bufmgr_ttm->fence_type = fence_type;
   bufmgr_ttm->fence_type_flush = fence_type_flush;
   bufmgr_ttm->cached_reloc_buf = NULL;
   bufmgr_ttm->cached_reloc_buf_data = NULL;

   /* lets go with one relocation per every four dwords - purely heuristic */
   bufmgr_ttm->max_relocs = batch_size / sizeof(uint32_t) / 2 - 2;

   bufmgr_ttm->bufmgr.bo_alloc = dri_ttm_alloc;
   bufmgr_ttm->bufmgr.bo_alloc_static = dri_ttm_alloc_static;
   bufmgr_ttm->bufmgr.bo_reference = dri_ttm_bo_reference;
   bufmgr_ttm->bufmgr.bo_unreference = dri_ttm_bo_unreference;
   bufmgr_ttm->bufmgr.bo_map = dri_ttm_bo_map;
   bufmgr_ttm->bufmgr.bo_unmap = dri_ttm_bo_unmap;
   bufmgr_ttm->bufmgr.fence_reference = dri_ttm_fence_reference;
   bufmgr_ttm->bufmgr.fence_unreference = dri_ttm_fence_unreference;
   bufmgr_ttm->bufmgr.fence_wait = dri_ttm_fence_wait;
   bufmgr_ttm->bufmgr.destroy = ddx_bufmgr_ttm_destroy;
   bufmgr_ttm->bufmgr.emit_reloc = dri_ttm_emit_reloc;
   bufmgr_ttm->bufmgr.process_relocs = dri_ttm_process_reloc;
   bufmgr_ttm->bufmgr.post_submit = dri_ttm_post_submit;
   return &bufmgr_ttm->bufmgr;
}

