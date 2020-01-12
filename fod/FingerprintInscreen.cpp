/*
 * Copyright (C) 2019 The LineageOS Project
 * Copyright (C) 2019-2020 The MoKee Open Source Project
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

#define LOG_TAG "InscreenService"

#include "FingerprintInscreen.h"
#include <android-base/logging.h>
#include <hidl/HidlTransportSupport.h>
#include <fstream>
#include <cmath>

#include <pthread.h>
#include <linux/input.h>
#include <sys/epoll.h>

#define EPOLLEVENTS 20

#define NOTIFY_FINGER_DOWN 1536
#define NOTIFY_FINGER_UP 1537
#define NOTIFY_UI_READY 1607
#define NOTIFY_UI_DISAPPER 1608
// #define NOTIFY_ENABLE_PAY_ENVIRONMENT 1609
// #define NOTIFY_DISABLE_PAY_ENVIRONMENT 1610

#define HBM_ENABLE_PATH "/sys/class/meizu/lcm/display/hbm"
#define BRIGHTNESS_PATH "/sys/class/backlight/panel0-backlight/brightness"

#define TOUCHPANAL_DEV_PATH "/dev/input/event2"

#define FOD_POS_X 149 * 3
#define FOD_POS_Y 604 * 3
#define FOD_SIZE 62 * 3

#define KEY_FOD 0x0272

namespace vendor {
namespace mokee {
namespace biometrics {
namespace fingerprint {
namespace inscreen {
namespace V1_0 {
namespace implementation {

/*
 * Write value to path and close file.
 */
template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}

template <typename T>
static T get(const std::string& path, const T& def) {
    std::ifstream file(path);
    T result;

    file >> result;
    return file.fail() ? def : result;
}

// Set by the signal handler to destroy the thread
volatile bool destroyThread;

void *work(void *);
void sighandler(int sig);

FingerprintInscreen::FingerprintInscreen() {
    this->mGoodixFpDaemon = IGoodixFingerprintDaemon::getService();

    destroyThread = false;
    signal(SIGUSR1, sighandler);

    if (pthread_create(&mPoll, NULL, work, this)) {
        LOG(ERROR) << "pthread creation failed: " << errno;
    }
}

Return<int32_t> FingerprintInscreen::getPositionX() {
    return FOD_POS_X;
}

Return<int32_t> FingerprintInscreen::getPositionY() {
    return FOD_POS_Y;
}

Return<int32_t> FingerprintInscreen::getSize() {
    return FOD_SIZE;
}

Return<void> FingerprintInscreen::onStartEnroll() {
    return Void();
}

Return<void> FingerprintInscreen::onFinishEnroll() {
    return Void();
}

Return<void> FingerprintInscreen::onPress() {
    set(HBM_ENABLE_PATH, 1);
    notifyHal(NOTIFY_FINGER_DOWN);
    return Void();
}

Return<void> FingerprintInscreen::onRelease() {
    set(HBM_ENABLE_PATH, 0);
    notifyHal(NOTIFY_FINGER_UP);
    return Void();
}

Return<void> FingerprintInscreen::onShowFODView() {
    notifyHal(NOTIFY_UI_READY);
    return Void();
}

Return<void> FingerprintInscreen::onHideFODView() {
    notifyHal(NOTIFY_UI_DISAPPER);
    return Void();
}

Return<bool> FingerprintInscreen::handleAcquired(int32_t, int32_t) {
    return false;
}

Return<bool> FingerprintInscreen::handleError(int32_t, int32_t) {
    return false;
}

Return<void> FingerprintInscreen::setLongPressEnabled(bool) {
    return Void();
}

Return<int32_t> FingerprintInscreen::getDimAmount(int32_t) {
    int brightness = get(BRIGHTNESS_PATH, 0);
    float alpha = 1.0 - pow(brightness / 255.0f, 0.455);
    return 255.0f * alpha;
}

Return<bool> FingerprintInscreen::shouldBoostBrightness() {
    return false;
}

Return<void> FingerprintInscreen::setCallback(const sp<IFingerprintInscreenCallback>& callback) {
    std::lock_guard<std::mutex> _lock(mCallbackLock);
    mCallback = callback;
    return Void();
}

void FingerprintInscreen::notifyKeyEvent(int value) {
    LOG(INFO) << "notifyKeyEvent: " << value;

    std::lock_guard<std::mutex> _lock(mCallbackLock);
    if (mCallback == nullptr) {
        return;
    }

    if (value) {
        Return<void> ret = mCallback->onFingerDown();
        if (!ret.isOk()) {
            LOG(ERROR) << "FingerDown() error: " << ret.description();
        }
    } else {
        Return<void> ret = mCallback->onFingerUp();
        if (!ret.isOk()) {
            LOG(ERROR) << "FingerUp() error: " << ret.description();
        }
    }
}

void FingerprintInscreen::notifyHal(int32_t cmd) {
    hidl_vec<int8_t> data;
    Return<void> ret = this->mGoodixFpDaemon->sendCommand(cmd, data, [&](int32_t, const hidl_vec<int8_t>&) {
    });
    if (!ret.isOk()) {
        LOG(ERROR) << "notifyHal(" << cmd << ") error: " << ret.description();
    }
}

void *work(void *param) {
    int epoll_fd, input_fd;
    struct epoll_event ev;
    struct input_event key_event;
    int nevents = 0;

    FingerprintInscreen *service = (FingerprintInscreen *)param;

    LOG(INFO) << "Creating thread";

    input_fd = open(TOUCHPANAL_DEV_PATH, O_RDONLY);
    if (input_fd < 0) {
        LOG(ERROR) << "Failed opening input dev: " << errno;
        return NULL;
    }

    ev.events = EPOLLIN;
    ev.data.fd = input_fd;

    epoll_fd = epoll_create(EPOLLEVENTS);
    if (epoll_fd == -1) {
        LOG(ERROR) << "Failed epoll_create: " << errno;
        goto error;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, input_fd, &ev) == -1) {
        LOG(ERROR) << "Failed epoll_ctl: " << errno;
        goto error;
    }

    while (!destroyThread) {
        struct epoll_event events[EPOLLEVENTS];

        nevents = epoll_wait(epoll_fd, events, EPOLLEVENTS, -1);
        if (nevents == -1) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "Failed epoll_wait: " << errno;
            break;
        }

        for (int i = 0; i < nevents; i++) {
            int ret;
            int fd = events[i].data.fd;
            lseek(fd, 0, SEEK_SET);
            ret = read(fd, &key_event, sizeof(key_event));
            if (ret && key_event.type == EV_KEY && key_event.code == KEY_FOD) {
                service->notifyKeyEvent(key_event.value);
            }
        }
    }

    LOG(INFO) << "Exiting worker thread";

error:
    close(input_fd);

    if (epoll_fd >= 0)
        close(epoll_fd);

    return NULL;
}

void sighandler(int sig) {
    if (sig == SIGUSR1) {
        destroyThread = true;
        LOG(INFO) << "Destroy set";
        return;
    }
    signal(SIGUSR1, sighandler);
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace inscreen
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace mokee
}  // namespace vendor
