/*
 * Copyright 2014 Google Inc. All Rights Reserved.
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

#include <tango-gl/conversions.h>

#include "tango-augmented-reality/scene.h"


namespace {
    // We want to represent the device properly with respect to the ground so we'll
    // add an offset in z to our origin. We'll set this offset to 1.3 meters based
    // on the average height of a human standing with a Tango device. This allows us
    // to place a grid roughly on the ground for most users.
    const glm::vec3 kHeightOffset = glm::vec3(0.0f, 0.0f, 0.0f);

    // Color of the motion tracking trajectory.
    const tango_gl::Color kTraceColor(0.22f, 0.28f, 0.67f);

    // Color of the ground grid.
    const tango_gl::Color kGridColor(0.85f, 0.85f, 0.85f);

    // Some property for the AR cube.
    const glm::quat kCubeRotation = glm::quat(0.0f, 0.0f, 1.0f, 0.0f);
    const glm::vec3 kCubePosition = glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 kCubeScale = glm::vec3(0.05f, 0.05f, 0.05f);
    const tango_gl::Color kCubeColor(1.0f, 0.f, 0.f);

    inline void Yuv2Rgb(uint8_t yValue, uint8_t uValue, uint8_t vValue, uint8_t *r,
                        uint8_t *g, uint8_t *b) {
        *r = yValue + (1.370705 * (vValue - 128));
        *g = yValue - (0.698001 * (vValue - 128)) - (0.337633 * (uValue - 128));
        *b = yValue + (1.732446 * (uValue - 128));
    }
}  // namespace

namespace tango_augmented_reality {

    Scene::Scene() { }

    Scene::~Scene() { }

    void Scene::InitGLContent() {

        // create drawable with rgb texture
        depth_frame = cv::Mat(1280, 720, CV_8UC3);
        depth_drawable_ = new DepthDrawable();
        glBindTexture(GL_TEXTURE_2D, depth_drawable_->GetTextureId());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, depth_frame.rows, depth_frame.cols, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);

        // create depth texture
        glGenTextures(1, &depth_frame_buffer_depth_texture_);
        glBindTexture(GL_TEXTURE_2D, depth_frame_buffer_depth_texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, depth_frame.rows, depth_frame.cols, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, NULL);

        // create frame buffer with color texture and depth
        glGenFramebuffers(1, &depth_frame_buffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, depth_frame_buffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, depth_drawable_->GetTextureId(), 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_frame_buffer_depth_texture_, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

        // check for errors of framebuffer
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("ERROR %d in fb %d ", depth_frame_buffer_);
        }

        // Allocating render camera and drawable object.
        // All of these objects are for visualization purposes.
        yuv_drawable_ = new YUVDrawable();
        gesture_camera_ = new tango_gl::GestureCamera();
        axis_ = new tango_gl::Axis();
        frustum_ = new tango_gl::Frustum();
        trace_ = new tango_gl::Trace();
        grid_ = new tango_gl::Grid();
        cube_ = new tango_gl::Cube();
        point_cloud_drawable_ = new PointCloudDrawable();

        trace_->SetColor(kTraceColor);
        grid_->SetColor(kGridColor);
        grid_->SetPosition(-kHeightOffset);

        cube_->SetPosition(kCubePosition);
        cube_->SetScale(kCubeScale);
        cube_->SetRotation(kCubeRotation);
        cube_->SetColor(kCubeColor);

        gesture_camera_->SetCameraType(tango_gl::GestureCamera::CameraType::kThirdPerson);
    }

    void Scene::DeleteResources() {
        delete gesture_camera_;
        delete yuv_drawable_;
        delete depth_drawable_;
        delete axis_;
        delete frustum_;
        delete trace_;
        delete grid_;
        delete cube_;
        delete point_cloud_drawable_;
    }

    void Scene::SetupViewPort(int x, int y, int w, int h) {
        if (h == 0) {
            LOGE("Setup graphic height not valid");
        }
        gesture_camera_->SetAspectRatio(static_cast<float>(w) / static_cast<float>(h));
        glViewport(x, y, w, h);
    }

    void Scene::Render(const glm::mat4 &cur_pose_transformation) {
        if (!is_yuv_texture_available_) {
            return;
        }

        ConvertYuvToRGBMat();
        BindRGBMatAsTexture();

        glEnable(GL_DEPTH_TEST);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

        glm::vec3 position = glm::vec3(cur_pose_transformation[3][0], cur_pose_transformation[3][1], cur_pose_transformation[3][2]);

        trace_->UpdateVertexArray(position);

        if (gesture_camera_->GetCameraType() == tango_gl::GestureCamera::CameraType::kFirstPerson) {
            // In first person mode, we directly control camera's motion.
            gesture_camera_->SetTransformationMatrix(cur_pose_transformation);
            // If it's first person view, we will render the video overlay in full
            // screen, so we passed identity matrix as view and projection matrix.
            glDisable(GL_DEPTH_TEST);
            yuv_drawable_->Render(glm::mat4(1.0f), glm::mat4(1.0f));
        } else {
            // In third person or top down more, we follow the camera movement.
            gesture_camera_->SetAnchorPosition(position);
            frustum_->SetTransformationMatrix(cur_pose_transformation);
            // Set the frustum scale to 4:3, this doesn't necessarily match the physical
            // camera's aspect ratio, this is just for visualization purposes.
            frustum_->SetScale(glm::vec3(1.0f, camera_image_plane_ratio_, image_plane_distance_));
            frustum_->Render(ar_camera_projection_matrix_, gesture_camera_->GetViewMatrix());
            axis_->SetTransformationMatrix(cur_pose_transformation);
            axis_->Render(ar_camera_projection_matrix_, gesture_camera_->GetViewMatrix());
            trace_->Render(ar_camera_projection_matrix_, gesture_camera_->GetViewMatrix());
            yuv_drawable_->Render(ar_camera_projection_matrix_, gesture_camera_->GetViewMatrix());
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST);

        // draw depth to framebuffer object
        glBindFramebuffer(GL_FRAMEBUFFER, depth_frame_buffer_);
        glClearColor(1.0, 1.0, 1.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        depth_mutex_.lock();
        point_cloud_drawable_->Render(gesture_camera_->GetProjectionMatrix(), gesture_camera_->GetViewMatrix(), point_cloud_transformation, vertices);
        depth_mutex_.unlock();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // copy depth to main framebuffer
        glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_frame_buffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 1280, 720, 0, 0, 1280, 720, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // render rest of drawables
        depth_drawable_->Render(glm::mat4(1.0f), glm::mat4(1.0f));
        grid_->Render(ar_camera_projection_matrix_, gesture_camera_->GetViewMatrix());
        cube_->Render(ar_camera_projection_matrix_, gesture_camera_->GetViewMatrix());

    }

    void Scene::SetCameraType(tango_gl::GestureCamera::CameraType camera_type) {
        gesture_camera_->SetCameraType(camera_type);

        depth_drawable_->SetParent(nullptr);
//        depth_drawable_->SetScale(glm::vec3(1.f, 1.f, 1.f));
//        depth_drawable_->SetPosition(glm::vec3(+0.f, -0., 0.0f));
        depth_drawable_->SetScale(glm::vec3(0.3f, 0.3f, 0.3f));
        depth_drawable_->SetPosition(glm::vec3(+0.6f, -0.6f, 0.0f));
        depth_drawable_->SetRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

        if (camera_type == tango_gl::GestureCamera::CameraType::kFirstPerson) {
            yuv_drawable_->SetParent(nullptr);
            yuv_drawable_->SetScale(glm::vec3(1.0f, 1.0f, 1.0f));
            yuv_drawable_->SetPosition(glm::vec3(0.0f, 0.0f, 0.0f));
            yuv_drawable_->SetRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        } else {
            yuv_drawable_->SetScale(glm::vec3(1.0f, camera_image_plane_ratio_, 1.0f));
            yuv_drawable_->SetRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            yuv_drawable_->SetPosition(glm::vec3(0.0f, 0.0f, -image_plane_distance_));
            yuv_drawable_->SetParent(axis_);
        }
    }

    void Scene::OnTouchEvent(int touch_count, tango_gl::GestureCamera::TouchEvent event, float x0, float y0, float x1, float y1) {
        gesture_camera_->OnTouchEvent(touch_count, event, x0, y0, x1, y1);
    }

    void Scene::OnFrameAvailable(const TangoImageBuffer *buffer) {
        if (yuv_drawable_->GetTextureId() == 0) {
            LOGE("yuv texture id not valid");
            return;
        }

        if (buffer->format != TANGO_HAL_PIXEL_FORMAT_YCrCb_420_SP) {
            LOGE("yuv texture format is not supported by this app");
            return;
        }

        // The memory needs to be allocated after we get the first frame because we
        // need to know the size of the image.
        if (!is_yuv_texture_available_) {
            yuv_width_ = buffer->width;
            yuv_height_ = buffer->height;
            LOGE("%d %d", yuv_width_, yuv_height_);
            uv_buffer_offset_ = yuv_width_ * yuv_height_;
            yuv_size_ = yuv_width_ * yuv_height_ + yuv_width_ * yuv_height_ / 2;

            // Reserve and resize the buffer size for RGB and YUV data.
            yuv_buffer_.resize(yuv_size_);
            yuv_temp_buffer_.resize(yuv_size_);
            rgb_buffer_.resize(yuv_width_ * yuv_height_ * 3);
            rgb_frame = cv::Mat(yuv_height_, yuv_width_, CV_8UC3);

            glBindTexture(GL_TEXTURE_2D, yuv_drawable_->GetTextureId());
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgb_frame.cols, rgb_frame.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame.ptr());

            is_yuv_texture_available_ = true;
        }

        std::lock_guard <std::mutex> lock(yuv_buffer_mutex_);
        memcpy(&yuv_temp_buffer_[0], buffer->data, yuv_size_);
        swap_buffer_signal_ = true;
    }

    void Scene::OnXYZijAvailable(const TangoXYZij *XYZ_ij) {
        std::vector <float> points;
        for (int i = 0; i < XYZ_ij->xyz_count; ++i) {
            points.push_back(XYZ_ij->xyz[i][0] * .9);
            points.push_back(XYZ_ij->xyz[i][1] * 1.2);
            points.push_back(XYZ_ij->xyz[i][2]);
        }
        depth_mutex_.lock();
        vertices = points;
        depth_mutex_.unlock();
    }

    void Scene::ConvertYuvToRGBMat() {
        {
            std::lock_guard <std::mutex> lock(yuv_buffer_mutex_);
            if (swap_buffer_signal_) {
                std::swap(yuv_buffer_, yuv_temp_buffer_);
                swap_buffer_signal_ = false;
            }
        }
        for (size_t i = 0; i < yuv_height_; ++i) {
            for (size_t j = 0; j < yuv_width_; ++j) {
                size_t x_index = j;
                if (j % 2 != 0) {
                    x_index = j - 1;
                }
                size_t rgb_index = (i * yuv_width_ + j) * 3;
                cv::Vec3b rgb_dot;
                Yuv2Rgb(yuv_buffer_[i * yuv_width_ + j],
                        yuv_buffer_[uv_buffer_offset_ + (i / 2) * yuv_width_ + x_index + 1],
                        yuv_buffer_[uv_buffer_offset_ + (i / 2) * yuv_width_ + x_index],
                        &rgb_dot[0], &rgb_dot[1], &rgb_dot[2]);
                rgb_frame.at<cv::Vec3b>(i, j) = rgb_dot;
            }
        }

    }

    void Scene::BindRGBMatAsTexture() {
        glBindTexture(GL_TEXTURE_2D, yuv_drawable_->GetTextureId());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgb_frame.cols, rgb_frame.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame.ptr());
    }

}  // namespace tango_augmented_reality