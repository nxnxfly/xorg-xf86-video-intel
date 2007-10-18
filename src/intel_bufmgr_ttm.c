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

#include "i915_drm.h"

#include "intel_bufmgr_ttm.h"

#define BUFMGR_DEBUG 0

struct intel_reloc_info
{
   uint32_t type;
   uint32_t reloc;
   uint32_t delta;                /* not needed? */
   uint32_t index;
   drm_handle_t handle;
};

struct intel_bo_node
{
    drmMMListHead head;
    drmBO *buf;
    struct drm_i915_op_arg bo_arg;
    unsigned long arg0;
    unsigned long arg1;
    void (*destroy)(void *);
    void *priv;
};

struct intel_bo_reloc_list
{
    drmMMListHead head;
    drmBO buf;
    uint32_t *relocs;
};

struct intel_bo_reloc_node
{
    drmMMListHead head;
    drm_handle_t handle;
    uint32_t nr_reloc_types;
    struct intel_bo_reloc_list type_list;
};

struct intel_bo_list {
    unsigned numCurrent;
    drmMMListHead list;
    void (*destroy)(void *node);
};

typedef struct _dri_bufmgr_ttm {
   dri_bufmgr bufmgr;

   int fd;
   unsigned int fence_type;
   unsigned int fence_type_flush;

   uint32_t max_relocs;
   /** ttm relocation list */
   struct intel_bo_list list;
   struct intel_bo_list reloc_list;

} dri_bufmgr_ttm;

typedef struct _dri_bo_ttm {
   dri_bo bo;

   int refcount;		/* Protected by bufmgr->mutex */
   drmBO drm_bo;
   const char *name;
} dri_bo_ttm;

typedef struct _dri_fence_ttm
{
   dri_fence fence;

   int refcount;		/* Protected by bufmgr->mutex */
   const char *name;
   drmFence drm_fence;
} dri_fence_ttm;


static void intel_bo_free_list(struct intel_bo_list *list)
{
    struct intel_bo_node *node;
    drmMMListHead *l;

    l = list->list.next;
    while(l != &list->list) {
	DRMLISTDEL(l);
	node = DRMLISTENTRY(struct intel_bo_node, l, head);
	list->destroy(node);
	l = list->list.next;
	list->numCurrent--;
    }
}

static void generic_destroy(void *nodep)
{
    free(nodep);
}

static int intel_create_bo_list(int numTarget, struct intel_bo_list *list, void (*destroy)(void *))
{
    DRMINITLISTHEAD(&list->list);
    list->numCurrent = 0;
    if (destroy)
        list->destroy = destroy;
    else
        list->destroy = generic_destroy;
    return 0;
}


static struct drm_i915_op_arg *
intel_setup_validate_list(int fd, struct intel_bo_list *list, struct intel_bo_list *reloc_list, uint32_t *count_p)
{
    struct intel_bo_node *node;
    struct intel_bo_reloc_node *rl_node;
    drmMMListHead *l, *rl;
    struct drm_i915_op_arg *arg, *first;
    struct drm_bo_op_req *req;
    uint64_t *prevNext = NULL;
    uint32_t count = 0;

    first = NULL;

    for (l = list->list.next; l != &list->list; l = l->next) {
        node = DRMLISTENTRY(struct intel_bo_node, l, head);

        arg = &node->bo_arg;
        req = &arg->d.req;

        if (!first)
            first = arg;

	if (prevNext)
	    *prevNext = (unsigned long) arg;

	memset(arg, 0, sizeof(*arg));
	prevNext = &arg->next;
	req->bo_req.handle = node->buf->handle;
	req->op = drm_bo_validate;
	req->bo_req.flags = node->arg0;
	req->bo_req.hint = 0;
	req->bo_req.mask = node->arg1;
	req->bo_req.fence_class = 0; /* Backwards compat. */
	arg->reloc_handle = 0;

	for (rl = reloc_list->list.next; rl != &reloc_list->list; rl = rl->next) {
	    rl_node = DRMLISTENTRY(struct intel_bo_reloc_node, rl, head);

	    if (rl_node->handle == node->buf->handle) {
		arg->reloc_handle = rl_node->type_list.buf.handle;
	    }
	}
	count++;
    }

    if (!first)
	return 0;

    *count_p = count;
    return first;
}

static void intel_free_validate_list(int fd, struct intel_bo_list *list)
{
    struct intel_bo_node *node;
    drmMMListHead *l;

    for (l = list->list.next; l != &list->list; l = l->next) {
        node = DRMLISTENTRY(struct intel_bo_node, l, head);    

	if (node->destroy)
	    (*node->destroy)(node->priv);

    }
}

static void intel_free_reloc_list(int fd, struct intel_bo_list *reloc_list)
{
    struct intel_bo_reloc_node *reloc_node;
    drmMMListHead *rl, *tmp;

    for (rl = reloc_list->list.next, tmp = rl->next; rl != &reloc_list->list; rl = tmp, tmp = rl->next) {
	reloc_node = DRMLISTENTRY(struct intel_bo_reloc_node, rl, head);

	DRMLISTDEL(rl);

	if (reloc_node->nr_reloc_types > 1) {
	    
	    /* TODO */
	}

	drmBOUnmap(fd, &reloc_node->type_list.buf);
	drmBOUnreference(fd, &reloc_node->type_list.buf);
	free(reloc_node);
    }
}

static int intel_add_validate_buffer(struct intel_bo_list *list, dri_bo *buf, unsigned flags,
				     unsigned mask, int *itemLoc, void (*destroy_cb)(void *))
{
    struct intel_bo_node *node, *cur;
    drmMMListHead *l;
    int count = 0;
    int ret = 0;
    drmBO *buf_bo = &((dri_bo_ttm *)buf)->drm_bo;
    cur = NULL;

    for (l = list->list.next; l != &list->list; l = l->next) {
	node = DRMLISTENTRY(struct intel_bo_node, l, head);
	if (node->buf->handle == buf_bo->handle) {
	    cur = node;
	    break;
	}
	count++;
    }
    
    if (!cur) {
	cur = drmMalloc(sizeof(*cur));
	if (!cur) {
	    return -ENOMEM;
	}
	cur->buf = buf_bo;
	cur->priv = buf;
	cur->arg0 = flags;
	cur->arg1 = mask;
	cur->destroy = destroy_cb;
	ret = 1;

	DRMLISTADDTAIL(&cur->head, &list->list);

    } else {
	unsigned memMask = (cur->arg1 | mask) & DRM_BO_MASK_MEM;
	unsigned memFlags = cur->arg0 & flags & memMask;
	
	if (!memFlags) {
	    return -EINVAL;
	}
	if (mask & cur->arg1 & ~DRM_BO_MASK_MEM  & (cur->arg0 ^ flags)) {
	    return -EINVAL;
	}
	cur->arg1 |= mask;
	cur->arg0 = memFlags | ((cur->arg0 | flags) & 
				cur->arg1 & ~DRM_BO_MASK_MEM);
    }
    *itemLoc = count;
    return ret;
}


#define RELOC_BUF_SIZE(x) ((I915_RELOC_HEADER + x * I915_RELOC0_STRIDE) * sizeof(uint32_t))

static int intel_create_new_reloc_type_list(int fd, struct intel_bo_reloc_list *cur_type, int max_relocs)
{
    int ret;
    
    /* should allocate a drmBO here */
    ret = drmBOCreate(fd, RELOC_BUF_SIZE(max_relocs), 0,
		      NULL,
		      DRM_BO_FLAG_MEM_LOCAL | DRM_BO_FLAG_READ | DRM_BO_FLAG_WRITE | DRM_BO_FLAG_MAPPABLE | DRM_BO_FLAG_CACHED,
		      0, &cur_type->buf);
    if (ret)
	return ret;
    
    ret = drmBOMap(fd, &cur_type->buf, DRM_BO_FLAG_READ | DRM_BO_FLAG_WRITE, 0, (void **)&cur_type->relocs);
    if (ret)
	return ret;
    return 0;
}


static int intel_add_validate_reloc(int fd, struct intel_bo_list *reloc_list, struct intel_reloc_info *reloc_info, uint32_t max_relocs)
{
    struct intel_bo_reloc_node *rl_node, *cur;
    drmMMListHead *rl, *l;
    int ret = 0;
    uint32_t *reloc_start;
    int num_relocs;
    struct intel_bo_reloc_list *cur_type;

    cur = NULL;

    for (rl = reloc_list->list.next; rl != &reloc_list->list; rl = rl->next) {
	rl_node = DRMLISTENTRY(struct intel_bo_reloc_node, rl, head);
	if (rl_node->handle == reloc_info->handle) {
	    cur = rl_node;
	    break;
	}
    }

    if (!cur) {

	cur = malloc(sizeof(*cur));
	if (!cur)
	    return -ENOMEM;

	cur->nr_reloc_types = 1;
	cur->handle = reloc_info->handle;
	cur_type = &cur->type_list;

	DRMINITLISTHEAD(&cur->type_list.head);
	ret = intel_create_new_reloc_type_list(fd, cur_type, max_relocs);
	if (ret) {
	    return -1;
	}
	DRMLISTADDTAIL(&cur->head, &reloc_list->list);

	cur_type->relocs[0] = 0 | (reloc_info->type << 16);
	cur_type->relocs[1] = 0; // next reloc buffer handle is 0

    } else {
	int found = 0;
	if ((cur->type_list.relocs[0] >> 16) == reloc_info->type) {
		cur_type = &cur->type_list;
		found = 1;
	} else {
	    for (l = cur->type_list.head.next; l != &cur->type_list.head; l = l->next) {
	        cur_type = DRMLISTENTRY(struct intel_bo_reloc_list, l, head);
	        if (((cur_type->relocs[0] >> 16) & 0xffff) == reloc_info->type)
	    	    found = 1;
		break;
	    }
        }
	
	/* didn't find the relocation type */
	if (!found) {
	    cur_type = malloc(sizeof(*cur_type));
	    if (!cur_type) {
		return -ENOMEM;
	    }
	    
	    ret = intel_create_new_reloc_type_list(fd, cur_type, max_relocs);
	    DRMLISTADDTAIL(&cur_type->head, &cur->type_list.head);

	    cur_type->relocs[0] = (reloc_info->type << 16);
	    cur_type->relocs[1] = 0;

	    cur->nr_reloc_types++;
	}
    }

    reloc_start = cur_type->relocs;

    num_relocs = (reloc_start[0] & 0xffff);

    reloc_start[num_relocs*I915_RELOC0_STRIDE + I915_RELOC_HEADER] = reloc_info->reloc;
    reloc_start[num_relocs*I915_RELOC0_STRIDE + I915_RELOC_HEADER+1] = reloc_info->delta;
    reloc_start[num_relocs*I915_RELOC0_STRIDE + I915_RELOC_HEADER+2] = reloc_info->index;
    reloc_start[0]++;
    if (((reloc_start[0] & 0xffff)) > (max_relocs)) {
	return -ENOMEM;
    }
    return 0;
}


#if 0
int
driFenceSignaled(DriFenceObject * fence, unsigned type)
{
   int signaled;
   int ret;

   if (fence == NULL)
      return GL_TRUE;

   ret = drmFenceSignaled(bufmgr_ttm->fd, &fence->fence, type, &signaled);
   BM_CKFATAL(ret);
   return signaled;
}
#endif

static dri_bo *
dri_ttm_alloc(dri_bufmgr *bufmgr, const char *name,
	      unsigned long size, unsigned int alignment,
	      unsigned int location_mask)
{
   dri_bufmgr_ttm *ttm_bufmgr;
   dri_bo_ttm *ttm_buf;
   unsigned int pageSize = getpagesize();
   int ret;
   unsigned int flags, hint;

   ttm_bufmgr = (dri_bufmgr_ttm *)bufmgr;

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

   ret = drmBOCreate(ttm_bufmgr->fd, size, alignment / pageSize,
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

#if BUFMGR_DEBUG
   fprintf(stderr, "bo_create: %p (%s)\n", &ttm_buf->bo, ttm_buf->name);
#endif

   return &ttm_buf->bo;
}

/* Our TTM backend doesn't allow creation of static buffers, as that requires
 * privelege for the non-fake case, and the lock in the fake case where we were
 * working around the X Server not creating buffers and passing handles to us.
 */
static dri_bo *
dri_ttm_alloc_static(dri_bufmgr *bufmgr, const char *name,
		     unsigned long offset, unsigned long size, void *virtual,
		     unsigned int location_mask)
{
   return NULL;
}

/** Returns a dri_bo wrapping the given buffer object handle.
 *
 * This can be used when one application needs to pass a buffer object
 * to another.
 */
dri_bo *
intel_ttm_bo_create_from_handle(dri_bufmgr *bufmgr, const char *name,
			      unsigned int handle)
{
   dri_bufmgr_ttm *ttm_bufmgr;
   dri_bo_ttm *ttm_buf;
   int ret;

   ttm_bufmgr = (dri_bufmgr_ttm *)bufmgr;

   ttm_buf = malloc(sizeof(*ttm_buf));
   if (!ttm_buf)
      return NULL;

   ret = drmBOReference(ttm_bufmgr->fd, handle, &ttm_buf->drm_bo);
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

#if BUFMGR_DEBUG
   fprintf(stderr, "bo_create_from_handle: %p %08x (%s)\n", &ttm_buf->bo, handle,
	   ttm_buf->name);
#endif

   return &ttm_buf->bo;
}

static void
dri_ttm_bo_reference(dri_bo *buf)
{
   dri_bo_ttm *ttm_buf = (dri_bo_ttm *)buf;

   ttm_buf->refcount++;
}

static void
dri_ttm_bo_unreference(dri_bo *buf)
{
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)buf->bufmgr;
   dri_bo_ttm *ttm_buf = (dri_bo_ttm *)buf;

   if (!buf)
      return;

   if (--ttm_buf->refcount == 0) {
      int ret;

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
dri_ttm_bo_map(dri_bo *buf, Bool write_enable)
{
   dri_bufmgr_ttm *bufmgr_ttm;
   dri_bo_ttm *ttm_buf = (dri_bo_ttm *)buf;
   unsigned int flags;

   bufmgr_ttm = (dri_bufmgr_ttm *)buf->bufmgr;

   flags = DRM_BO_FLAG_READ;
   if (write_enable)
       flags |= DRM_BO_FLAG_WRITE;

   assert(buf->virtual == NULL);

#if BUFMGR_DEBUG
   fprintf(stderr, "bo_map: %p (%s)\n", &ttm_buf->bo, ttm_buf->name);
#endif

   return drmBOMap(bufmgr_ttm->fd, &ttm_buf->drm_bo, flags, 0, &buf->virtual);
}

static int
dri_ttm_bo_unmap(dri_bo *buf)
{
   dri_bufmgr_ttm *bufmgr_ttm;
   dri_bo_ttm *ttm_buf = (dri_bo_ttm *)buf;

   if (buf == NULL)
      return 0;

   bufmgr_ttm = (dri_bufmgr_ttm *)buf->bufmgr;

   assert(buf->virtual != NULL);

   buf->virtual = NULL;

#if BUFMGR_DEBUG
   fprintf(stderr, "bo_unmap: %p (%s)\n", &ttm_buf->bo, ttm_buf->name);
#endif

   return drmBOUnmap(bufmgr_ttm->fd, &ttm_buf->drm_bo);
}

/* Returns a dri_bo wrapping the given buffer object handle.
 *
 * This can be used when one application needs to pass a buffer object
 * to another.
 */
dri_fence *
intel_ttm_fence_create_from_arg(dri_bufmgr *bufmgr, const char *name,
				drm_fence_arg_t *arg)
{
   dri_bufmgr_ttm *ttm_bufmgr;
   dri_fence_ttm *ttm_fence;

   ttm_bufmgr = (dri_bufmgr_ttm *)bufmgr;

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
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)fence->bufmgr;

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
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)fence->bufmgr;
   int ret;

   ret = drmFenceWait(bufmgr_ttm->fd, 0, &fence_ttm->drm_fence, 0);
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
dri_bufmgr_ttm_destroy(dri_bufmgr *bufmgr)
{
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)bufmgr;

   intel_bo_free_list(&bufmgr_ttm->list);
   intel_bo_free_list(&bufmgr_ttm->reloc_list);

   free(bufmgr);
}


static void intel_dribo_destroy_callback(void *priv)
{
   dri_bo *dribo = priv;
   
   if (dribo) {
     dri_bo_unreference(dribo);
   }
}

static void
dri_ttm_emit_reloc(dri_bo *batch_buf, uint32_t flags, uint32_t delta, uint32_t offset,
		    dri_bo *relocatee)
{
   dri_bo_ttm *ttm_buf = (dri_bo_ttm *)batch_buf;
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)batch_buf->bufmgr;
   int newItem;
   struct intel_reloc_info reloc;
   int mask;
   int ret;

   mask = DRM_BO_MASK_MEM;
   mask |= flags & (DRM_BO_FLAG_READ | DRM_BO_FLAG_WRITE | DRM_BO_FLAG_EXE);

   ret = intel_add_validate_buffer(&bufmgr_ttm->list, relocatee, flags, mask, &newItem, intel_dribo_destroy_callback);
   if (ret < 0)
     return;

   if (ret == 1) {
      dri_bo_reference(relocatee);
   }

   reloc.type = I915_RELOC_TYPE_0;
   reloc.reloc = offset;
   reloc.delta = delta;
   reloc.index = newItem;
   reloc.handle = ttm_buf->drm_bo.handle;

   intel_add_validate_reloc(bufmgr_ttm->fd, &bufmgr_ttm->reloc_list, &reloc, bufmgr_ttm->max_relocs);
   return;
}


static void *
dri_ttm_process_reloc(dri_bo *batch_buf, uint32_t *count)
{
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)batch_buf->bufmgr;
   void *ptr;
   int itemLoc;

   dri_bo_unmap(batch_buf);

   intel_add_validate_buffer(&bufmgr_ttm->list, batch_buf, DRM_BO_FLAG_MEM_TT | DRM_BO_FLAG_EXE,
			     DRM_BO_MASK_MEM | DRM_BO_FLAG_EXE, &itemLoc, NULL);

   ptr = intel_setup_validate_list(bufmgr_ttm->fd, &bufmgr_ttm->list, &bufmgr_ttm->reloc_list, count);

   return ptr;
}

static void
dri_ttm_post_submit(dri_bo *batch_buf, dri_fence **last_fence)
{
   dri_bufmgr_ttm *bufmgr_ttm = (dri_bufmgr_ttm *)batch_buf->bufmgr;

   intel_free_validate_list(bufmgr_ttm->fd, &bufmgr_ttm->list);
   intel_free_reloc_list(bufmgr_ttm->fd, &bufmgr_ttm->reloc_list);

   intel_bo_free_list(&bufmgr_ttm->list);
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
dri_bufmgr *
intel_bufmgr_ttm_init(int fd, unsigned int fence_type,
		      unsigned int fence_type_flush, int batch_size)
{
   dri_bufmgr_ttm *bufmgr_ttm;

   bufmgr_ttm = malloc(sizeof(*bufmgr_ttm));
   bufmgr_ttm->fd = fd;
   bufmgr_ttm->fence_type = fence_type;
   bufmgr_ttm->fence_type_flush = fence_type_flush;

   /* lets go with one relocation per every four dwords - purely heuristic */
   bufmgr_ttm->max_relocs = batch_size / sizeof(uint32_t) / 4;

   intel_create_bo_list(10, &bufmgr_ttm->list, NULL);
   intel_create_bo_list(1, &bufmgr_ttm->reloc_list, NULL);

   bufmgr_ttm->bufmgr.bo_alloc = dri_ttm_alloc;
   bufmgr_ttm->bufmgr.bo_alloc_static = dri_ttm_alloc_static;
   bufmgr_ttm->bufmgr.bo_reference = dri_ttm_bo_reference;
   bufmgr_ttm->bufmgr.bo_unreference = dri_ttm_bo_unreference;
   bufmgr_ttm->bufmgr.bo_map = dri_ttm_bo_map;
   bufmgr_ttm->bufmgr.bo_unmap = dri_ttm_bo_unmap;
   bufmgr_ttm->bufmgr.fence_reference = dri_ttm_fence_reference;
   bufmgr_ttm->bufmgr.fence_unreference = dri_ttm_fence_unreference;
   bufmgr_ttm->bufmgr.fence_wait = dri_ttm_fence_wait;
   bufmgr_ttm->bufmgr.destroy = dri_bufmgr_ttm_destroy;
   bufmgr_ttm->bufmgr.emit_reloc = dri_ttm_emit_reloc;
   bufmgr_ttm->bufmgr.process_relocs = dri_ttm_process_reloc;
   bufmgr_ttm->bufmgr.post_submit = dri_ttm_post_submit;
   return &bufmgr_ttm->bufmgr;
}

