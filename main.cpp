#include "stdio.h"
#include <fcntl.h>
#include "gbm.h"
#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "egl.h"
#include "eglext.h"
#include "gl2.h"
#include <string>
#include <cstring>

struct DRMFBState
{
    int fd;
    gbm_bo* bo;
    uint32_t fb_id;
};
drmModeConnector* connector_;
drmModeEncoder* encoder_;
drmModeCrtcPtr crtc_;
drmModeModeInfo* mode_;
gbm_device* dev_;
gbm_surface* surface_;
gbm_bo* bo_;
DRMFBState* fb_;
drmModeRes *resources_;
int fd_;

EGLNativeDisplayType native_display_;
EGLNativeWindowType native_window_;
EGLDisplay egl_display_;
EGLContext egl_context_;
EGLSurface egl_surface_;
EGLConfig egl_config_;
bool crtc_set_=false;

int init_drm()
{
	fd_ = open("/dev/dri/card0", O_RDWR);
	resources_ =  drmModeGetResources(fd_);
	if (!resources_) {
        printf("drmModeGetResources failed\n");
        return false;
    }
	
	// Find a connected connector
    for (int c = 0; c < resources_->count_connectors; c++) {
        connector_ = drmModeGetConnector(fd_, resources_->connectors[c]);
        if (DRM_MODE_CONNECTED == connector_->connection) {
            break;
        }
        drmModeFreeConnector(connector_);
        connector_ = 0;
    }
	if (!connector_) {
        printf("Failed to find a suitable connector\n");
        return false;
    }

	// Find the best resolution (we will always operate full-screen).
    unsigned int bestArea(0);
    for (int m = 0; m < connector_->count_modes; m++) {
        drmModeModeInfo* curMode = &connector_->modes[m];
        unsigned int curArea = curMode->hdisplay * curMode->vdisplay;
        if (curArea > bestArea) {
            mode_ = curMode;
            bestArea = curArea;
        }
    }
    if (!mode_) {
        printf("Failed to find a suitable mode\n");
        return false;
    }

    // Find a suitable encoder
    for (int e = 0; e < resources_->count_encoders; e++) {
        bool found = false;
        encoder_ = drmModeGetEncoder(fd_, resources_->encoders[e]);
        for (int ce = 0; ce < connector_->count_encoders; ce++) {
            if (encoder_ && encoder_->encoder_id == connector_->encoders[ce]) {
                found = true;
                break;
            }
        }
        if (found)
            break;
        drmModeFreeEncoder(encoder_);
        encoder_ = 0;
    }

    // If encoder is not connected to the connector try to find
    // a suitable one
    if (!encoder_) {
        for (int e = 0; e < connector_->count_encoders; e++) {
            encoder_ = drmModeGetEncoder(fd_, connector_->encoders[e]);
            for (int c = 0; c < resources_->count_crtcs; c++) {
                if (encoder_->possible_crtcs & (1 << c)) {
                    encoder_->crtc_id = resources_->crtcs[c];
                    break;
                }
            }
            if (encoder_->crtc_id) {
                break;
            }

            drmModeFreeEncoder(encoder_);
            encoder_ = 0;
        }
    }

    if (!encoder_) {
        printf("Failed to find a suitable encoder\n");
        return false;
    }
}
int init_gl()
{
	dev_ = gbm_create_device(fd_);
    if (!dev_) {
        printf("Failed to create GBM device\n");
        return false;
    }

    surface_ = gbm_surface_create(dev_, mode_->hdisplay, mode_->vdisplay,
                                  GBM_FORMAT_XRGB8888,
                                  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!surface_) {
        printf("Failed to create GBM surface\n");
        return false;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));

    if (get_platform_display != NULL) {
        egl_display_ = get_platform_display(
            EGL_PLATFORM_GBM_KHR,
            dev_,
            NULL);
    }

    if (!egl_display_) {
        printf("eglGetPlatformDisplayEXT() failed with error: 0x%x\n",
                eglGetError());
    }

	if (!eglInitialize(egl_display_, NULL, NULL)) {
    	printf("eglInitialize() failed with error: 0x%x\n", eglGetError());
        egl_display_ = 0;
        return false;
    }
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    	printf("Failed to bind either EGL_OPENGL_ES_API APIs.\n");
        return false;
    }
    egl_context_ = eglCreateContext(egl_display_, NULL,
                                    EGL_NO_CONTEXT, context_attribs);
    if (!egl_context_) {
        printf("eglCreateContext() failed with error: 0x%x\n",
                   eglGetError());
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 1,
        EGL_DEPTH_SIZE, 1,
        EGL_STENCIL_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint num_configs(0);
    if (!eglChooseConfig(egl_display_, config_attribs, 0, 0, &num_configs)) {
        printf("eglChooseConfig() (count query) failed with error: %d\n",
                   eglGetError());
        return false;
    }
    if (num_configs == 0) {
        printf("eglChooseConfig() didn't return any configs\n");
        return false;
    }
        // Get all the matching configs
    EGLConfig configs[num_configs];
    if (!eglChooseConfig(egl_display_, config_attribs,configs,num_configs, &num_configs))
    {
        printf("eglChooseConfig() failed with error: %d\n",
                     eglGetError());
        return false;
    }
    egl_config_ = configs[0];

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, surface_, 0);
    if (!egl_surface_) {
        printf("eglCreateWindowSurface failed with error: 0x%x\n", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
        printf("eglMakeCurrent failed with error: 0x%x\n", eglGetError());
        return false;
    }

    if (!eglSwapInterval(egl_display_, 0)) {
        printf("** Failed to set swap interval. Results may be bounded above by refresh rate.\n");
    }
}
void fb_destroy_callback(gbm_bo* bo, void* data)
{
    DRMFBState* fb = reinterpret_cast<DRMFBState*>(data);
    if (fb && fb->fb_id) {
        drmModeRmFB(fb->fd, fb->fb_id);
    }
    delete fb;
    gbm_device* dev = gbm_bo_get_device(bo);
    printf("Got GBM device handle %p from buffer object\n", dev);
}
DRMFBState* fb_get_from_bo(gbm_bo* bo)
{
    DRMFBState* fb = reinterpret_cast<DRMFBState*>(gbm_bo_get_user_data(bo));
    if (fb) {
        return fb;
    }

    unsigned int width = gbm_bo_get_width(bo);
    unsigned int height = gbm_bo_get_height(bo);
    unsigned int stride = gbm_bo_get_stride(bo);
    unsigned int handle = gbm_bo_get_handle(bo).u32;
    unsigned int fb_id(0);
    int status = drmModeAddFB(fd_, width, height, 24, 32, stride, handle, &fb_id);
    if (status < 0) {
        printf("Failed to create FB: %d\n", status);
        return 0;
    }

    fb = new DRMFBState();
    fb->fd = fd_;
    fb->bo = bo;
    fb->fb_id = fb_id;

    gbm_bo_set_user_data(bo, fb, fb_destroy_callback);
    return fb;
}
void page_flip_handler(int/*  fd */, unsigned int /* frame */, unsigned int /* sec */, unsigned int /* usec */, void* data)
{
    unsigned int* waiting = reinterpret_cast<unsigned int*>(data);
    *waiting = 0;
}

void flip()
{
    gbm_bo* next = gbm_surface_lock_front_buffer(surface_);
    fb_ = fb_get_from_bo(next);
    unsigned int waiting(1);

    if (!crtc_set_) {
        int status = drmModeSetCrtc(fd_, encoder_->crtc_id, fb_->fb_id, 0, 0,
                                    &connector_->connector_id, 1, mode_);
        if (status >= 0) {
            crtc_set_ = true;
            bo_ = next;
        }
        else {
            printf("Failed to set crtc: %d\n", status);
        }
        return;
    }

    int status = drmModePageFlip(fd_, encoder_->crtc_id, fb_->fb_id,
                                 DRM_MODE_PAGE_FLIP_EVENT, &waiting);
    if (status < 0) {
        printf("Failed to enqueue page flip: %d\n", status);
        return;
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    drmEventContext evCtx;
    memset(&evCtx, 0, sizeof(evCtx));
    evCtx.version = 2;
    evCtx.page_flip_handler = page_flip_handler;

    while (waiting) {
        status = select(fd_ + 1, &fds, 0, 0, 0);
        if (status < 0) {
            // Most of the time, select() will return an error because the
            // user pressed Ctrl-C.  So, only print out a message in debug
            // mode, and just check for the likely condition and release
            // the current buffer object before getting out.
            printf("Error in select\n");
            if (1) {
                gbm_surface_release_buffer(surface_, bo_);
                bo_ = next;
            }
            return;
        }
        drmHandleEvent(fd_, &evCtx);
    }

    gbm_surface_release_buffer(surface_, bo_);
    bo_ = next;
}
int gldraw()
{
    eglSwapBuffers(egl_display_, egl_surface_);
    flip();
}
int main()
{
    init_drm();
    init_gl();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gldraw();
}
