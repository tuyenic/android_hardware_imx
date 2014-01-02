/*
 * Copyright (C) 2009-2014 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <hardware/hardware.h>

#include <fcntl.h>
#include <errno.h>
#include <math.h>

#define HWC_REMOVE_DEPRECATED_VERSIONS 1
#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <utils/threads.h>
#include <hardware/hwcomposer.h>
#include <hardware_legacy/uevent.h>
#include <utils/StrongPointer.h>

#include <linux/mxcfb.h>
#include <linux/ioctl.h>
#include <EGL/egl.h>
#include "gralloc_priv.h"
#include "hwc_context.h"
#include "hwc_vsync.h"
#include "hwc_uevent.h"
#include "hwc_display.h"
#include <g2d.h>
#include <system/graphics.h>

extern "C" int get_aligned_size(buffer_handle_t hnd, int *width, int *height);

static bool validateRect(hwc_rect_t& rect)
{
    if ((rect.left < 0) || (rect.top < 0) || (rect.right < 0) ||
        (rect.bottom < 0) || (rect.right - rect.left <= 0) ||
        (rect.bottom - rect.top <= 0)) {
        return false;
    }
    return true;
}

static enum g2d_format convertFormat(int format)
{
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
            return G2D_RGBA8888;
        case HAL_PIXEL_FORMAT_RGBX_8888:
            return G2D_RGBX8888;
        case HAL_PIXEL_FORMAT_RGB_565:
            return G2D_RGB565;
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return G2D_BGRA8888;

        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            return G2D_NV21;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            return G2D_NV12;

        case HAL_PIXEL_FORMAT_YCbCr_420_P:
            return G2D_I420;
        case HAL_PIXEL_FORMAT_YV12:
            return G2D_YV12;

        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            return G2D_NV16;
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
            return G2D_YUYV;

        default:
            ALOGE("unsupported format:0x%x", format);
            return G2D_RGBA8888;
    }
}

static enum g2d_rotation convertRotation(int transform)
{
    switch (transform) {
        case 0:
            return G2D_ROTATION_0;
        case HAL_TRANSFORM_ROT_90:
            return G2D_ROTATION_90;
        case HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V:
            return G2D_ROTATION_180;
        case HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V |
             HAL_TRANSFORM_ROT_90:
            return G2D_ROTATION_270;
        case HAL_TRANSFORM_FLIP_H:
            return G2D_FLIP_H;
        case HAL_TRANSFORM_FLIP_V:
            return G2D_FLIP_V;
        default:
            return G2D_ROTATION_0;
    }
}

static int convertBlending(int blending, struct g2d_surface& src,
                        struct g2d_surface& dst)
{
    switch (blending) {
        case HWC_BLENDING_PREMULT:
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        case HWC_BLENDING_COVERAGE:
            src.blendfunc = G2D_SRC_ALPHA;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        case HWC_BLENDING_DIM:
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        default:
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;
    }

    return 0;
}

static int setG2dSurface(struct g2d_surface& surface,
             struct private_handle_t *handle, hwc_rect_t& rect)
{
    surface.format = convertFormat(handle->format);
    surface.planes[0] = handle->phys;
    surface.stride = handle->flags >> 16;
    switch (surface.format) {
        case G2D_RGB565: {
            int alignHeight = 0;
            int ret = get_aligned_size(handle, NULL, &alignHeight);
            if (ret != 0) {
                alignHeight = (handle->height + 3) & ~0x3;
            }
            surface.planes[0] += surface.stride * 2 * (alignHeight - handle->height);
            } break;
        case G2D_RGBA8888:
        case G2D_BGRA8888:
        case G2D_RGBX8888: {
            int alignHeight = 0;
            int ret = get_aligned_size(handle, NULL, &alignHeight);
            if (ret != 0) {
                alignHeight = (handle->height + 3) & ~0x3;
            }
            surface.planes[0] += surface.stride * 4 * (alignHeight - handle->height);
            } break;

        case G2D_YUYV:
            break;

        case G2D_NV16:
        case G2D_NV12:
        case G2D_NV21: {
            int stride = surface.stride;
            surface.planes[1] = handle->phys + stride * handle->height;
            } break;

        case G2D_I420:
        case G2D_YV12: {
            int stride = surface.stride;
            int c_stride = (stride/2+15)/16*16;
            surface.planes[1] = handle->phys + stride * handle->height;
            surface.planes[2] = surface.planes[1] + c_stride * handle->height;
            } break;

        default:
            ALOGI("does not support format:%d", surface.format);
            break;
    }
    surface.left = rect.left;
    surface.top = rect.top;
    surface.right = rect.right;
    surface.bottom = rect.bottom;
    surface.width = handle->width;
    surface.height = handle->height;

    return 0;
}

static inline int min(int a, int b) {
    return (a<b) ? a : b;
}

static inline int max(int a, int b) {
    return (a>b) ? a : b;
}

void convertScalerToInt(hwc_frect_t& in, hwc_rect_t& out)
{
    out.left = (int)(ceilf(in.left));
    out.top = (int)(ceilf(in.top));
    out.right = (int)(floorf(in.right));
    out.bottom = (int)(floorf(in.bottom));
}

static bool isEmpty(const hwc_rect_t & hs)
{
    return (hs.left > hs.right || hs.top > hs.bottom);
}

static bool isIntersect(const hwc_rect_t* lhs, const hwc_rect_t* rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }

    if ((lhs->right < rhs->left) || (lhs->bottom < rhs->top) ||
        (lhs->left > rhs->right) || (lhs->top > rhs->bottom)) {
        return false;
    }

    return true;
}

static void intersect(hwc_rect_t* out, const hwc_rect_t* lhs,
            const hwc_rect_t* rhs)
{
    if (out == NULL || lhs == NULL || rhs == NULL) {
        return;
    }

    out->left = max(lhs->left, rhs->left);
    out->top = max(lhs->top, rhs->top);
    out->right = min(lhs->right, rhs->right);
    out->bottom = min(lhs->bottom, rhs->bottom);
}

static void unite(hwc_rect_t* out, const hwc_rect_t* lhs,
            const hwc_rect_t* rhs)
{
    if (out == NULL || lhs == NULL || rhs == NULL) {
        return;
    }

    out->left = min(lhs->left, rhs->left);
    out->top = min(lhs->top, rhs->top);
    out->right = max(lhs->right, rhs->right);
    out->bottom = max(lhs->bottom, rhs->bottom);
}

static void subtract(hwc_region_t* out, const hwc_rect_t& lhs,
            const hwc_rect_t& rhs)
{
    if (out == NULL/* || lhs == NULL || rhs == NULL*/) {
        return;
    }

    if (!isIntersect(&lhs, &rhs)) {
        ((hwc_rect_t*)out->rects)[out->numRects].left = lhs.left;
        ((hwc_rect_t*)out->rects)[out->numRects].top = lhs.top;
        ((hwc_rect_t*)out->rects)[out->numRects].right = lhs.right;
        ((hwc_rect_t*)out->rects)[out->numRects].bottom = lhs.bottom;
    }
    else if (!isEmpty(lhs)) {
        if (lhs.top < rhs.top) { // top rect
            ((hwc_rect_t*)out->rects)[out->numRects].left = lhs.left;
            ((hwc_rect_t*)out->rects)[out->numRects].top = lhs.top;
            ((hwc_rect_t*)out->rects)[out->numRects].right = lhs.right;
            ((hwc_rect_t*)out->rects)[out->numRects].bottom = rhs.top;
            out->numRects ++;
        }

        const int32_t top = max(lhs.top, rhs.top);
        const int32_t bot = min(lhs.bottom, rhs.bottom);
        if (top < bot) {
            if (lhs.left < rhs.left) { // left-side rect
                ((hwc_rect_t*)out->rects)[out->numRects].left = lhs.left;
                ((hwc_rect_t*)out->rects)[out->numRects].top = top;
                ((hwc_rect_t*)out->rects)[out->numRects].right = rhs.left;
                ((hwc_rect_t*)out->rects)[out->numRects].bottom = bot;
                out->numRects ++;
            }

            if (lhs.right > rhs.right) { // right-side rect
                ((hwc_rect_t*)out->rects)[out->numRects].left = rhs.right;
                ((hwc_rect_t*)out->rects)[out->numRects].top = top;
                ((hwc_rect_t*)out->rects)[out->numRects].right = lhs.right;
                ((hwc_rect_t*)out->rects)[out->numRects].bottom = bot;
                out->numRects ++;
            }
        }

        if (lhs.bottom > rhs.bottom) { // bottom rect
            ((hwc_rect_t*)out->rects)[out->numRects].left = lhs.left;
            ((hwc_rect_t*)out->rects)[out->numRects].top = rhs.bottom;
            ((hwc_rect_t*)out->rects)[out->numRects].right = lhs.right;
            ((hwc_rect_t*)out->rects)[out->numRects].bottom = lhs.bottom;
            out->numRects ++;
        }
    }
}

static void clipRects(hwc_rect_t& src, hwc_rect_t& dst,
                     const hwc_rect_t& dstClip, int rotation)
{
    hwc_rect_t drect = dst;
    hwc_rect_t srect = src;
    int32_t srcW, srcH, dstW, dstH, deltaX, deltaY;
    intersect(&dst, &dstClip, &drect);

    //rotate 90
    if (rotation == HAL_TRANSFORM_ROT_90) {
        dstW = drect.bottom - drect.top;
        dstH = drect.right - drect.left;
        deltaX = dst.top - drect.top;
        deltaY = -dst.right + drect.right;
    }
    //virtical flip
    else if (rotation == HAL_TRANSFORM_FLIP_V) {
        dstW = drect.right - drect.left;
        dstH = drect.bottom - drect.top;
        deltaX = dst.left - drect.left;
        deltaY = -dst.bottom + drect.bottom;
    }
    //horizontal flip
    else if (rotation == HAL_TRANSFORM_FLIP_H) {
        dstW = drect.right - drect.left;
        dstH = drect.bottom - drect.top;
        deltaX = -dst.right + drect.right;
        deltaY = dst.top - drect.top;
    }
    //rotate 270
    else if (rotation == (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V |
             HAL_TRANSFORM_ROT_90)) {
        dstW = drect.bottom - drect.top;
        dstH = drect.right - drect.left;
        deltaY = dst.left - drect.left;
        deltaX = -dst.bottom + drect.bottom;
    }
    //rotate 180
    else if (rotation == (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V)) {
        dstW = drect.right - drect.left;
        dstH = drect.bottom - drect.top;
        deltaY = -dst.bottom + drect.bottom;
        deltaX = -dst.right + drect.right;
    }
    //rotate 0
    else {
        dstW = drect.right - drect.left;
        dstH = drect.bottom - drect.top;
        deltaX = dst.left - drect.left;
        deltaY = dst.top - drect.top;
    }
    srcW = srect.right - srect.left;
    srcH = srect.bottom - srect.top;

    deltaX = deltaX * srcW / dstW;
    deltaY = deltaY * srcH / dstH;

    src.left = deltaX + srect.left;
    src.top = deltaY + srect.top;
    if (rotation & HAL_TRANSFORM_ROT_90) {
        src.right = src.left + (dstClip.bottom - dstClip.top)* srcW / dstW;
        src.bottom = src.top + (dstClip.right - dstClip.left)* srcH / dstH;
    }
    else {
        src.right = src.left + (dstClip.right - dstClip.left) * srcW / dstW;
        src.bottom = src.top + (dstClip.bottom - dstClip.top) * srcH / dstH;
    }
}

int hwc_composite(struct hwc_context_t* ctx, hwc_layer_1_t* layer,
         struct private_handle_t *dstHandle, hwc_rect_t* swap, bool firstLayer)
{
    if (ctx == NULL || ctx->g2d_handle == NULL || layer == NULL || dstHandle == NULL) {
        ALOGE("%s: invalid parameters", __FUNCTION__);
        return -EINVAL;
    }

    //sourceCrop change with version.
    hwc_rect_t srect;
    convertScalerToInt(layer->sourceCropf, srect);
    if (!validateRect(srect) && layer->blending != HWC_BLENDING_DIM) {
        ALOGE("%s: invalid sourceCrop(l:%d,t:%d,r:%d,b:%d)", __FUNCTION__,
                 srect.left, srect.top, srect.right, srect.bottom);
        return -EINVAL;
    }

    hwc_rect_t drect = layer->displayFrame;
    if (!validateRect(drect)) {
        ALOGE("%s: invalid displayFrame(l:%d,t:%d,r:%d,b:%d)", __FUNCTION__,
                 drect.left, drect.top, drect.right, drect.bottom);
        return -EINVAL;
    }

    struct g2d_surface dSurface;
    memset(&dSurface, 0, sizeof(dSurface));

    for (size_t i=0; i<layer->visibleRegionScreen.numRects; i++) {
        convertScalerToInt(layer->sourceCropf, srect);
        drect = layer->displayFrame;
        if (!validateRect((hwc_rect_t&)layer->visibleRegionScreen.rects[i])) {
            ALOGI("invalid clip rect");
            continue;
        }

        hwc_rect_t clip = layer->visibleRegionScreen.rects[i];
        if (swap != NULL && !isEmpty(*swap) && isIntersect(swap, &clip)) {
            intersect(&clip, &clip, swap);
        }

        clipRects(srect, drect, clip, layer->transform);
        if (!validateRect(srect) && layer->blending != HWC_BLENDING_DIM) {
            ALOGV("%s: invalid srect(l:%d,t:%d,r:%d,b:%d)", __FUNCTION__,
                    srect.left, srect.top, srect.right, srect.bottom);
            hwc_rect_t src;
            convertScalerToInt(layer->sourceCropf, src);
            hwc_rect_t& vis = (hwc_rect_t&)layer->visibleRegionScreen.rects[i];
            hwc_rect_t& dis = layer->displayFrame;
            ALOGV("sourceCrop(l:%d,t:%d,r:%d,b:%d), visible(l:%d,t:%d,r:%d,b:%d), "
                    "display(l:%d,t:%d,r:%d,b:%d)",
                    src.left, src.top, src.right, src.bottom,
                    vis.left, vis.top, vis.right, vis.bottom,
                    dis.left, dis.top, dis.right, dis.bottom);
            if (swap != NULL) {
                ALOGV("swap(l:%d,t:%d,r:%d,b:%d)",
                 swap->left, swap->top, swap->right, swap->bottom);
            }
            continue;
        }
        if (!validateRect(drect)) {
            ALOGI("%s: invalid drect(l:%d,t:%d,r:%d,b:%d)", __FUNCTION__,
                    drect.left, drect.top, drect.right, drect.bottom);
            continue;
        }

        ALOGV("draw: src(l:%d,t:%d,r:%d,b:%d), dst(l:%d,t:%d,r:%d,b:%d)",
                srect.left, srect.top, srect.right, srect.bottom,
                drect.left, drect.top, drect.right, drect.bottom);

        setG2dSurface(dSurface, dstHandle, drect);
        dSurface.rot = convertRotation(layer->transform);

        struct g2d_surface sSurface;
        memset(&sSurface, 0, sizeof(sSurface));

        if (layer->blending != HWC_BLENDING_DIM) {
            struct private_handle_t *priv_handle;
            priv_handle = (struct private_handle_t *)(layer->handle);
            setG2dSurface(sSurface, priv_handle, srect);
        }
        else {
            sSurface.clrcolor = 0xff000000;
            sSurface.format = G2D_RGBA8888;
        }
        ALOGV("blit rot:%d, blending:%d, alpha:%d", layer->transform,
                layer->blending, layer->planeAlpha);

        if (firstLayer && layer->blending == HWC_BLENDING_DIM) {
            continue;
        }

        if (!firstLayer) {
            convertBlending(layer->blending, sSurface, dSurface);
        }
        sSurface.global_alpha = layer->planeAlpha;

        if (layer->blending != HWC_BLENDING_NONE && !firstLayer) {
            g2d_enable(ctx->g2d_handle, G2D_GLOBAL_ALPHA);
            if (layer->blending == HWC_BLENDING_DIM) {
                ALOGV("enable blend dim");
                g2d_enable(ctx->g2d_handle, G2D_BLEND_DIM);
            }
            else {
                g2d_enable(ctx->g2d_handle, G2D_BLEND);
            }
        }

        g2d_blit(ctx->g2d_handle, &sSurface, &dSurface);

        if (layer->blending != HWC_BLENDING_NONE && !firstLayer) {
            if (layer->blending == HWC_BLENDING_DIM) {
                g2d_disable(ctx->g2d_handle, G2D_BLEND_DIM);
            }
            else {
                g2d_disable(ctx->g2d_handle, G2D_BLEND);
            }
            g2d_disable(ctx->g2d_handle, G2D_GLOBAL_ALPHA);
        }
    }

    return 0;
}

int hwc_copyBack(struct hwc_context_t* ctx, struct private_handle_t *dstHandle,
                struct private_handle_t *srcHandle, int swapIndex, int disp)
{
    if (ctx == NULL || ctx->g2d_handle == NULL || srcHandle == NULL || dstHandle == NULL) {
        ALOGE("%s: invalid parameters", __FUNCTION__);
        return -EINVAL;
    }

    hwc_rect_t& swapRect = ctx->mDispInfo[disp].mSwapRect[swapIndex];
    hwc_rect_t uniRect;
    memset(&uniRect, 0, sizeof(uniRect));
    int index = (swapIndex + 1)%HWC_MAX_FRAMEBUFFER;
    while (index != swapIndex) {
        unite(&uniRect, &uniRect, &(ctx->mDispInfo[disp].mSwapRect[index]));
        index = (index + 1)%HWC_MAX_FRAMEBUFFER;
    }

    #define MAX_HWC_RECTS 4
    typedef struct hwc_reg {
        hwc_region_t reg;
        hwc_rect_t rect[MAX_HWC_RECTS];
    } hwc_reg_t;
    hwc_reg_t resReg;
    resReg.reg.numRects = 0;
    resReg.reg.rects = resReg.rect;
    hwc_region_t &resRegion = resReg.reg;
    subtract(&resRegion, uniRect, swapRect);

    struct g2d_surface dSurface;
    memset(&dSurface, 0, sizeof(dSurface));
    for (size_t i=0; i<resRegion.numRects; i++) {
        ALOGV("%s(l:%d,t:%d,r:%d,b:%d)", __FUNCTION__,
            resRegion.rects[i].left, resRegion.rects[i].top,
            resRegion.rects[i].right, resRegion.rects[i].bottom);
        if (!validateRect((hwc_rect_t&)resRegion.rects[i])) {
            ALOGI("invalid rect");
            continue;
        }

        setG2dSurface(dSurface, dstHandle, (hwc_rect_t&)resRegion.rects[i]);
        struct g2d_surface sSurface;
        memset(&sSurface, 0, sizeof(sSurface));
        setG2dSurface(sSurface, srcHandle, (hwc_rect_t&)resRegion.rects[i]);
        g2d_blit(ctx->g2d_handle, &sSurface, &dSurface);
    }

    return 0;
}

bool hwc_hasSameContent(struct hwc_context_t* ctx, int src,
            int dst, hwc_display_contents_1_t** lists)
{
    if (ctx == NULL || lists == NULL || src == dst) {
        ALOGE("%s invalid ctx, lists or src==dst", __FUNCTION__);
        return false;
    }

    hwc_display_contents_1_t* sList = lists[src];
    hwc_display_contents_1_t* dList = lists[dst];
    if (sList->numHwLayers != dList->numHwLayers) {
        return false;
    }

    int numLayers = sList->numHwLayers;
    for (int i=0; i<numLayers-1; i++) {
        if (sList->hwLayers[i].handle != dList->hwLayers[i].handle) {
            return false;
        }
    }

    return true;
}

int hwc_resize(struct hwc_context_t* ctx, struct private_handle_t *dstHandle,
                    struct private_handle_t *srcHandle)
{
    if (ctx == NULL || ctx->g2d_handle == NULL || srcHandle == NULL || dstHandle == NULL) {
        ALOGE("%s invalid ctx", __FUNCTION__);
        return -EINVAL;
    }

    hwc_rect_t srect;
    srect.left = 0;
    srect.top = 0;
    srect.right = srcHandle->width;
    srect.bottom = srcHandle->height;

    hwc_rect_t drect;
    int deltaW = 0, deltaH = 0;
    int dstW = dstHandle->width;
    int dstH = dstHandle->height;
    if (dstW * srcHandle->height >= dstH * srcHandle->width) {
        dstW = dstH * srcHandle->width / srcHandle->height;
    }
    else {
        dstH = dstW * srcHandle->height / srcHandle->width;
    }

    deltaW = dstHandle->width - dstW;
    deltaH = dstHandle->height - dstH;
    drect.left = deltaW / 2;
    drect.top = deltaH / 2;
    drect.right = drect.left + dstW;
    drect.bottom = drect.top + dstH;

    g2d_surface sSurface, dSurface;
    memset(&sSurface, 0, sizeof(sSurface));
    memset(&dSurface, 0, sizeof(dSurface));
    setG2dSurface(sSurface, srcHandle, srect);
    setG2dSurface(dSurface, dstHandle, drect);

    g2d_blit(ctx->g2d_handle, &sSurface, &dSurface);

    return 0;
}

int hwc_updateSwapRect(struct hwc_context_t* ctx, int disp,
                 android_native_buffer_t* nbuf)
{
    if (ctx == NULL) {
        ALOGE("%s invalid ctx", __FUNCTION__);
        return -EINVAL;
    }

    int index = ctx->mDispInfo[disp].mSwapIndex;
    ctx->mDispInfo[disp].mSwapIndex = (index + 1)%HWC_MAX_FRAMEBUFFER;
    hwc_rect_t& swapRect = ctx->mDispInfo[disp].mSwapRect[index];
    if (nbuf != NULL) {
        int origin = (int) nbuf->common.reserved[0];
        int size = (int) nbuf->common.reserved[1];
        if (origin != 0 && size != 0) {
            swapRect.left = origin >> 16;
            swapRect.top = origin & 0xFFFF;
            swapRect.right = swapRect.left + (size >> 16);
            swapRect.bottom = swapRect.top + (size & 0xFFFF);
            ALOGV("swapRect:(l:%d,t:%d,r:%d,b:%d)",
                swapRect.left, swapRect.top, swapRect.right, swapRect.bottom);
            return index;
        }
    }

    swapRect.left = 0;
    swapRect.top = 0;
    swapRect.right = ctx->mDispInfo[disp].xres;
    swapRect.bottom = ctx->mDispInfo[disp].yres;
    ALOGV("swapRect:(l:%d,t:%d,r:%d,b:%d)",
        swapRect.left, swapRect.top, swapRect.right, swapRect.bottom);
    return index;
}

int hwc_clearWormHole(struct hwc_context_t* ctx, struct private_handle_t *dstHandle,
                    hwc_region_t &hole)
{
    if (ctx == NULL || dstHandle == NULL || hole.rects == NULL) {
        return -EINVAL;
    }

    struct g2d_surface surface;
    memset(&surface, 0, sizeof(surface));
    for (size_t i=0; i<hole.numRects; i++) {
        hwc_rect_t& rect = (hwc_rect_t&)hole.rects[i];
        if (!validateRect(rect)) {
            continue;
        }

        setG2dSurface(surface, dstHandle, rect);
        surface.clrcolor = 0xff << 24;
        g2d_clear(ctx->g2d_handle, &surface);
    }

    return 0;
}

int hwc_clearRect(struct hwc_context_t* ctx, struct private_handle_t *dstHandle,
                    hwc_rect_t &rect)
{
    if (ctx == NULL || dstHandle == NULL) {
        return -EINVAL;
    }

    if (!validateRect(rect)) {
        return -EINVAL;
    }

    struct g2d_surface surface;
    memset(&surface, 0, sizeof(surface));

    setG2dSurface(surface, dstHandle, rect);
    surface.clrcolor = 0xff << 24;
    g2d_clear(ctx->g2d_handle, &surface);

    return 0;
}
