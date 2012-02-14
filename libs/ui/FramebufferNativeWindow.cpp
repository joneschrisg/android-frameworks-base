/* 
**
** Copyright 2007 The Android Open Source Project
** Copyright (c) 2010-2011 Code Aurora Forum. All rights reserved.
**
** Licensed under the Apache License Version 2.0(the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing software 
** distributed under the License is distributed on an "AS IS" BASIS 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#define LOG_TAG "FramebufferNativeWindow"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <utils/threads.h>
#include <utils/RefBase.h>

#include <ui/Rect.h>
#include <ui/FramebufferNativeWindow.h>
#include <ui/GraphicLog.h>

#include <EGL/egl.h>

#include <pixelflinger/format.h>
#include <pixelflinger/pixelflinger.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <private/ui/android_natives_priv.h>

// ----------------------------------------------------------------------------
namespace android {
// ----------------------------------------------------------------------------

class NativeBuffer 
    : public EGLNativeBase<
        android_native_buffer_t, 
        NativeBuffer, 
        LightRefBase<NativeBuffer> >
{
public:
    NativeBuffer(int w, int h, int f, int u) : BASE() {
        android_native_buffer_t::width  = w;
        android_native_buffer_t::height = h;
        android_native_buffer_t::format = f;
        android_native_buffer_t::usage  = u;
    }
private:
    friend class LightRefBase<NativeBuffer>;    
    ~NativeBuffer() { }; // this class cannot be overloaded
};


/*
 * This implements the (main) framebuffer management. This class is used
 * mostly by SurfaceFlinger, but also by command line GL application.
 * 
 * In fact this is an implementation of ANativeWindow on top of
 * the framebuffer.
 * 
 * Currently it is pretty simple, it manages only two buffers (the front and 
 * back buffer).
 * 
 */

FramebufferNativeWindow::FramebufferNativeWindow() 
    : BASE(), fbDev(0), grDev(0), mUpdateOnDemand(false)
{
    hw_module_t const* module;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
        int stride;
        int err;
        err = framebuffer_open(module, &fbDev);
        LOGE_IF(err, "couldn't open framebuffer HAL (%s)", strerror(-err));
        
        err = gralloc_open(module, &grDev);
        LOGE_IF(err, "couldn't open gralloc HAL (%s)", strerror(-err));

        // bail out if we can't initialize the modules
        if (!fbDev || !grDev) {
            if (grDev) {
                gralloc_close(grDev);
                grDev = 0;
            }
            if (fbDev) {
                framebuffer_close(fbDev);
                fbDev = 0;
            }
            return;
        }
        
        mUpdateOnDemand = (fbDev->setUpdateRect != 0);
        
        // initialize the buffer FIFO
#ifdef ENABLE_GRALLOC_NUMFB
        mNumBuffers = fbDev->numFramebuffers;
#else
        mNumBuffers = 2;
#endif
        mNumFreeBuffers = mNumBuffers;
        mBufferHead = 0;
        LOGD("mNumBuffers = %d", mNumBuffers);
        for(int i = 0; i < mNumBuffers; i++) {
            buffers[i] = new NativeBuffer(
                fbDev->width, fbDev->height, fbDev->format, GRALLOC_USAGE_HW_FB);

            err = grDev->alloc(grDev,
                fbDev->width, fbDev->height, fbDev->format, 
                GRALLOC_USAGE_HW_FB, &buffers[i]->handle, &buffers[i]->stride);

            LOGE_IF(err, "fb buffer %d allocation failed w=%d, h=%d, err=%s",
                i, fbDev->width, fbDev->height, strerror(-err));
        }

        const_cast<uint32_t&>(ANativeWindow::flags) = fbDev->flags; 
        const_cast<float&>(ANativeWindow::xdpi) = fbDev->xdpi;
        const_cast<float&>(ANativeWindow::ydpi) = fbDev->ydpi;
        const_cast<int&>(ANativeWindow::minSwapInterval) = 
            fbDev->minSwapInterval;
        const_cast<int&>(ANativeWindow::maxSwapInterval) = 
            fbDev->maxSwapInterval;
    } else {
        LOGE("Couldn't get gralloc module");
    }

    ANativeWindow::setSwapInterval = setSwapInterval;
    ANativeWindow::dequeueBuffer = dequeueBuffer;
    ANativeWindow::lockBuffer = lockBuffer;
    ANativeWindow::queueBuffer = queueBuffer;
    ANativeWindow::cancelBuffer = NULL;
    ANativeWindow::query = query;
    ANativeWindow::perform = perform;
}

FramebufferNativeWindow::~FramebufferNativeWindow() 
{
    LOGE_IF(!grDev, "~FramebufferNativeWindow [grDev==NULL]");

    Mutex::Autolock _l(mutex);

    if (grDev) {
        for(int i = 0; i < mNumBuffers; i++) {
            if (buffers[i] != NULL) {
                grDev->free(grDev, buffers[i]->handle);
            }
        }
        gralloc_close(grDev);
        grDev = 0;
    }

    if (fbDev) {
        framebuffer_close(fbDev);
        fbDev = 0;
    }
}

status_t FramebufferNativeWindow::setUpdateRectangle(const Rect& r) 
{
    if (!mUpdateOnDemand || !fbDev) {
        return INVALID_OPERATION;
    }
    return fbDev->setUpdateRect(fbDev, r.left, r.top, r.width(), r.height());
}

status_t FramebufferNativeWindow::compositionComplete()
{
    if (fbDev && fbDev->compositionComplete) {
        return fbDev->compositionComplete(fbDev);
    }
    return INVALID_OPERATION;
}

int FramebufferNativeWindow::setSwapInterval(
        ANativeWindow* window, int interval) 
{
    framebuffer_device_t* fb = getSelf(window)->fbDev;
    return fb != NULL ? fb->setSwapInterval(fb, interval) : -EINVAL;
}

// only for debugging / logging
int FramebufferNativeWindow::getCurrentBufferIndex() const
{
    Mutex::Autolock _l(mutex);
    LOGE_IF(!grDev, "[RACE] FramebufferNativeWindow::getCurrentBufferIndex");
    const int index = mCurrentBufferIndex;
    return index;
}

int FramebufferNativeWindow::dequeueBuffer(ANativeWindow* window, 
        android_native_buffer_t** buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    framebuffer_device_t* fb;
    {
        Mutex::Autolock _l(self->mutex);
        LOGE_IF(!self->grDev, "[RACE] FramebufferNativeWindow::dequeueBuffer");
        fb = self->fbDev;
        if (!fb)
            return NO_INIT;
    }
    int index = self->mBufferHead;

    GraphicLog& logger(GraphicLog::getInstance());
    logger.log(GraphicLog::SF_FB_DEQUEUE_BEFORE, index);

#ifdef ENABLE_GRALLOC_NUMFB
    /* This function is supposed to check whether there is atleast
       one free buffer available.
       fb->dequeueBuffer will check for the BufferHead being
       available, which needs to be checked explicitly for
       more than 2 buffer case, because in that case, fb_post
       is not going to wait for BufferHead to be available, it will
       wait just for the next buffer to be available.

       For 2 framebuffers, there will always
       be at least one buffer free. BufferHead being available
       is already being checked in lockBuffer function.
    */
    if (fb->numFramebuffers > 2) {
        fb->dequeueBuffer(fb, index);
    }
#endif

    /* The buffer is available, return it */
    Mutex::Autolock _l(self->mutex);
    self->mBufferHead++;
    if (self->mBufferHead >= self->mNumBuffers)
        self->mBufferHead = 0;

    // get this buffer
    self->mNumFreeBuffers--;
    self->mCurrentBufferIndex = index;

    *buffer = self->buffers[index].get();

    logger.log(GraphicLog::SF_FB_DEQUEUE_AFTER, index);
    return 0;
}

int FramebufferNativeWindow::lockBuffer(ANativeWindow* window, 
        android_native_buffer_t* buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    LOGE_IF(!self->grDev, "[RACE] FramebufferNativeWindow::lockBuffer");

    const int index = self->mCurrentBufferIndex;
    GraphicLog& logger(GraphicLog::getInstance());
    logger.log(GraphicLog::SF_FB_LOCK_BEFORE, index);

    // wait that the buffer we're locking is not front anymore
    while (self->front == buffer) {
        self->mCondition.wait(self->mutex);
    }

    logger.log(GraphicLog::SF_FB_LOCK_AFTER, index);
    return NO_ERROR;
}


int FramebufferNativeWindow::queueBuffer(ANativeWindow* window, 
        android_native_buffer_t* buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    LOGE_IF(!self->grDev, "[RACE] FramebufferNativeWindow::queueBuffer");

    framebuffer_device_t* fb = self->fbDev;
    if (!fb)
        return NO_INIT;
    buffer_handle_t handle = static_cast<NativeBuffer*>(buffer)->handle;

    const int index = self->mCurrentBufferIndex;
    GraphicLog& logger(GraphicLog::getInstance());
    logger.log(GraphicLog::SF_FB_POST_BEFORE, index);

    int res = fb->post(fb, handle);

    logger.log(GraphicLog::SF_FB_POST_AFTER, index);

    self->front = static_cast<NativeBuffer*>(buffer);
    self->mNumFreeBuffers++;
    self->mCondition.broadcast();
    return res;
}

int FramebufferNativeWindow::query(ANativeWindow* window,
        int what, int* value) 
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    LOGE_IF(!self->grDev, "[RACE] FramebufferNativeWindow::query");

    framebuffer_device_t* fb = self->fbDev;
    if (fb) {
        switch (what) {
        case NATIVE_WINDOW_WIDTH:
            *value = fb->width;
            return NO_ERROR;
        case NATIVE_WINDOW_HEIGHT:
            *value = fb->height;
            return NO_ERROR;
        case NATIVE_WINDOW_FORMAT:
            *value = fb->format;
            return NO_ERROR;
        }
    }
    *value = 0;
    return BAD_VALUE;
}

int FramebufferNativeWindow::perform(ANativeWindow* window,
        int operation, ...)
{
    switch (operation) {
        case NATIVE_WINDOW_SET_USAGE:
        case NATIVE_WINDOW_CONNECT:
        case NATIVE_WINDOW_DISCONNECT:
            break;
        default:
            return NAME_NOT_FOUND;
    }
    return NO_ERROR;
}

// ----------------------------------------------------------------------------
}; // namespace android
// ----------------------------------------------------------------------------

using namespace android;

EGLNativeWindowType android_createDisplaySurface(void)
{
    FramebufferNativeWindow* w;
    w = new FramebufferNativeWindow();
    if (w->getDevice() == NULL) {
        // get a ref so it can be destroyed when we exit this block
        sp<FramebufferNativeWindow> ref(w);
        return NULL;
    }
    return (EGLNativeWindowType)w;
}
