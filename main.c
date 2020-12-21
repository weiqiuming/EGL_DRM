#include "stdio.h"
#include <fcntl.h>
#include "gbm.h"
#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "egl.h"
#include "eglext.h"

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
        // Get all the matching configs
    EGLConfig configs[num_configs];
    if (!eglChooseConfig(egl_display_, config_attribs, &configs.front(),
                         num_configs, &num_configs))
    {
        printf("eglChooseConfig() failed with error: %d\n",
                     eglGetError());
        return false;
    }
    egl_config_ = configs[0];

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, native_window_, 0);
    if (!egl_surface_) {
        printf("eglCreateWindowSurface failed with error: 0x%x\n", eglGetError());
        return false;
    }
}
int main()
{


}
