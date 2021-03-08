#include "gbm.h"
#include "stdio.h"
#include <drm.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
//#include "epoxy/gl.h"
//#include "epoxy/egl.h"

#include "EGL/egl.h"
#include "EGL/eglext.h"
#include "GL/gl.h"
#include "GL/glext.h"

struct DRMFBState {
  int fd;
  struct gbm_bo *bo;
  uint32_t fb_id;
};
struct Image_OES_FBO {
  struct gbm_bo *bo;
  uint32_t fb_id;
  GLuint fbo;
};
drmModeConnector *connector_;
drmModeEncoder *encoder_;
drmModeCrtcPtr crtc_;
drmModeModeInfo *mode_;
struct gbm_device *dev_;
struct gbm_bo *bo_;
struct DRMFBState *fb_;
drmModeRes *resources_;
int fd_;

EGLNativeDisplayType native_display_;
EGLNativeWindowType native_window_;
EGLDisplay egl_display_;
EGLContext egl_context_;
EGLConfig egl_config_;
bool crtc_set_ = false;
bool need_init = true;

// fbo
struct Image_OES_FBO oes_fbo[2];
unsigned char cur_fbo = 0;

int init_drm() {
  fd_ = open("/dev/dri/card0", O_RDWR);
  resources_ = drmModeGetResources(fd_);
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
  unsigned int bestArea = 0;
  for (int m = 0; m < connector_->count_modes; m++) {
    drmModeModeInfo *curMode = &connector_->modes[m];
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

  return true;
}

int init_gl_context() {
  dev_ = gbm_create_device(fd_);
  if (!dev_) {
    printf("Failed to create GBM device\n");
    return false;
  }

  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
          "eglGetPlatformDisplayEXT");

  if (get_platform_display != NULL) {
    egl_display_ = get_platform_display(EGL_PLATFORM_GBM_KHR, dev_, NULL);
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
  static const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  if (!eglBindAPI(EGL_OPENGL_API)) {
    printf("Failed to bind either EGL_OPENGL_API APIs.\n");
    return false;
  }
  egl_context_ =
      eglCreateContext(egl_display_, NULL, EGL_NO_CONTEXT, context_attribs);
  if (!egl_context_) {
    printf("eglCreateContext() failed with error: 0x%x\n", eglGetError());
    return false;
  }

  return true;
}

void fb_destroy_callback(struct gbm_bo *bo, void *data) {
  struct DRMFBState *fb = (struct DRMFBState *)(data);
  if (fb && fb->fb_id) {
    drmModeRmFB(fb->fd, fb->fb_id);
  }
  fb = NULL;
  struct gbm_device *dev = gbm_bo_get_device(bo);
  printf("Got GBM device handle %p from buffer object\n", dev);
}

struct DRMFBState *fb_get_from_bo(struct gbm_bo *bo) {
  struct DRMFBState *fb = (struct DRMFBState *)(gbm_bo_get_user_data(bo));
  if (fb) {
    return fb;
  }

  unsigned int width = gbm_bo_get_width(bo);
  unsigned int height = gbm_bo_get_height(bo);
  unsigned int stride = gbm_bo_get_stride(bo);
  unsigned int handle = gbm_bo_get_handle(bo).u32;
  unsigned int fb_id = 0;
  int status = drmModeAddFB(fd_, width, height, 24, 32, stride, handle, &fb_id);
  if (status < 0) {
    printf("Failed to create FB: %d\n", status);
    return 0;
  }

  fb->fd = fd_;
  fb->bo = bo;
  fb->fb_id = fb_id;

  gbm_bo_set_user_data(bo, fb, fb_destroy_callback);
  return fb;
}

void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                       unsigned int usec, void *data) {
  unsigned int *waiting = (unsigned int *)(data);
  *waiting = 0;
}

static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

int init_egl_fbo() {
  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  if (!glEGLImageTargetTexture2DOES || !eglCreateImageKHR) {
    printf("get oes and image_khr erro\n");
  }

  eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (!eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      egl_context_)) {
    printf("eglMakeCurrent failed with error: 0x%x\n", eglGetError());
    return false;
  }
  // create fbo
  for (size_t i = 0; i < 2; i++) {
    oes_fbo[i].bo = gbm_bo_create(dev_, mode_->hdisplay, mode_->vdisplay,
                                  GBM_FORMAT_ABGR8888,
                                  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    EGLImageKHR image =
        eglCreateImageKHR(egl_display_, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                          oes_fbo[i].bo, NULL);
    if (image == EGL_NO_IMAGE_KHR) {
      printf("egl no image khr\n");
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &oes_fbo[i].fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, oes_fbo[i].fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    int status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      printf("glCheckFramebufferStatus failed with error: 0x%x\n",
             eglGetError());
      return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    unsigned int width = gbm_bo_get_width(oes_fbo[i].bo);
    unsigned int height = gbm_bo_get_height(oes_fbo[i].bo);
    unsigned int stride = gbm_bo_get_stride(oes_fbo[i].bo);
    unsigned int handle = gbm_bo_get_handle(oes_fbo[i].bo).u32;
    unsigned int fb_id = 0;
    status = drmModeAddFB(fd_, width, height, 24, 32, stride, handle, &fb_id);
    if (status < 0) {
      printf("Failed to create FB: %d\n", status);
      return 0;
    }
    oes_fbo[i].fb_id = fb_id;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, oes_fbo[cur_fbo].fbo);

  return true;
}

void swapfbo() {
  glFlush();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  cur_fbo = (~cur_fbo) & 1;
  glBindFramebuffer(GL_FRAMEBUFFER, oes_fbo[cur_fbo].fbo);
}
void scanout() {
  unsigned char scanout_fbo = (~cur_fbo) & 1;
  ;
  unsigned int waiting = 1;

  if (!crtc_set_) {
    int status =
        drmModeSetCrtc(fd_, encoder_->crtc_id, oes_fbo[scanout_fbo].fb_id, 0, 0,
                       &connector_->connector_id, 1, mode_);
    if (status >= 0) {
      crtc_set_ = true;
    } else {
      printf("Failed to set crtc: %d\n", status);
    }
    return;
  }

  int status =
      drmModePageFlip(fd_, encoder_->crtc_id, oes_fbo[scanout_fbo].fb_id,
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
      return;
    }
    drmHandleEvent(fd_, &evCtx);
  }
}
void gl_draw_fbo() {
  if (need_init == true) {
    init_egl_fbo();
    need_init = false;
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  // draw
  glClear(GL_COLOR_BUFFER_BIT);
  // scanout
  swapfbo();
  scanout();
}
int main() {
  init_drm();
  init_gl_context();

  unsigned int draw_num = 20;
  unsigned int i = 0;

  while (i < draw_num) {
    gl_draw_fbo();
    if (i > draw_num / 2) {
      glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    } else {
      glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
    }
    i++;
  }

  gbm_device_destroy(dev_);
}
