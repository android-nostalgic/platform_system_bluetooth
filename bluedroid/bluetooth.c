/*
 * Copyright (C) 2008 Google Inc.
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

#define LOG_TAG "bluedroid"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <bluedroid/bluetooth.h>

#ifndef HCI_DEV_ID
#define HCI_DEV_ID 0
#endif

#define HCID_START_DELAY_SEC   5
#define HCID_STOP_DELAY_USEC 500000

#define MIN(x,y) (((x)<(y))?(x):(y))

#define USE_UGLY_POWER_INTERFACE 1
#if USE_UGLY_POWER_INTERFACE

#define BLUETOOTH_POWER_PATH "/sys/module/board_trout/parameters/bluetooth_power_on"

static int set_bluetooth_power(int on) {
    int ret = -1;
    int sz;
    const char buffer = (on ? 'Y' : 'N');
    int fd = open(BLUETOOTH_POWER_PATH, O_WRONLY);

    if (fd == -1) {
        LOGE("Can't open %s for write: %s (%d)", BLUETOOTH_POWER_PATH,
             strerror(errno), errno);
        goto out;
    }
    sz = write(fd, &buffer, 1);
    if (sz != 1) {
        LOGE("Can't write to %s: %s (%d)", BLUETOOTH_POWER_PATH,
             strerror(errno), errno);
        goto out;
    }
    ret = 0;

out:
    if (fd >= 0) close(fd);
    return ret;
}

static int check_bluetooth_power() {
    int ret = -1;
    int sz;
    char buffer;
    int fd = open(BLUETOOTH_POWER_PATH, O_RDONLY);

    if (fd == -1) {
        LOGE("Can't open %s for read: %s (%d)", BLUETOOTH_POWER_PATH,
             strerror(errno), errno);
        goto out;
    }
    sz = read(fd, &buffer, 1);
    if (sz != 1) {
        LOGE("Can't read from %s: %s (%d)", BLUETOOTH_POWER_PATH,
             strerror(errno), errno);
        goto out;
    }

    switch (buffer) {
    case 'Y':
        ret = 1;
        break;
    case 'N':
        ret = 0;
        break;
    }

out:
    if (fd >= 0) close(fd);
    return ret;
}

#else

static int rfkill_id = -1;
static char *rfkill_state_path = NULL;


static int init_rfkill() {
    char path[64];
    char buf[16];
    int fd;
    int sz;
    int id;
    for (id = 0; ; id++) {
        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", id);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            LOGW("open(%s) failed: %s (%d)\n", path, strerror(errno), errno);
            return -1;
        }
        sz = read(fd, &buf, sizeof(buf));
        close(fd);
        if (sz >= 9 && memcmp(buf, "bluetooth", 9) == 0) {
            rfkill_id = id;
            break;
        }
    }

    asprintf(&rfkill_state_path, "/sys/class/rfkill/rfkill%d/state", rfkill_id);
    return 0;
}

static int check_bluetooth_power() {
    int sz;
    int fd = -1;
    int ret = -1;
    char buffer;

    if (rfkill_id == -1) {
        if (init_rfkill()) goto out;
    }

    fd = open(rfkill_state_path, O_RDONLY);
    if (fd < 0) {
        LOGE("open(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
        goto out;
    }
    sz = read(fd, &buffer, 1);
    if (sz != 1) {
        LOGE("read(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
        goto out;
    }

    switch (buffer) {
    case '1':
        ret = 1;
        break;
    case '0':
        ret = 0;
        break;
    }

out:
    if (fd >= 0) close(fd);
    return ret;
}

static int set_bluetooth_power(int on) {
    int sz;
    int fd = -1;
    int ret = -1;
    const char buffer = (on ? '1' : '0');

    if (rfkill_id == -1) {
        if (init_rfkill()) goto out;
    }

    fd = open(rfkill_state_path, O_WRONLY);
    if (fd < 0) {
        LOGE("open(%s) for write failed: %s (%d)", rfkill_state_path,
             strerror(errno), errno);
        goto out;
    }
    sz = write(fd, &buffer, 1);
    if (sz < 0) {
        LOGE("write(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
        goto out;
    }
    ret = 0;

out:
    if (fd >= 0) close(fd);
    return ret;
}
#endif

static inline int create_hci_sock() {
    int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sk < 0) {
        LOGE("Failed to create bluetooth hci socket: %s (%d)",
             strerror(errno), errno);
    }
    return sk;
}

int bt_enable() {
    LOGV(__FUNCTION__);

    int ret = -1;
    int hci_sock = -1;
    int attempt;

    if (set_bluetooth_power(1) < 0) goto out;

    LOGI("Starting hciattach daemon");
    if (property_set("ctl.start", "hciattach") < 0) {
        LOGE("Failed to start hciattach");
        goto out;
    }

    // Try for 10 seconds, this can only succeed once hciattach has sent the
    // firmware and then turned on hci device via HCIUARTSETPROTO ioctl
    for (attempt = 1000; attempt > 0;  attempt--) {
        hci_sock = create_hci_sock();
        if (hci_sock < 0) goto out;

        if (!ioctl(hci_sock, HCIDEVUP, HCI_DEV_ID)) {
            break;
        }
        close(hci_sock);
        usleep(10000);  // 10 ms retry delay
    }
    if (attempt == 0) {
        LOGE("%s: Timeout waiting for HCI device to come up", __FUNCTION__);
        goto out;
    }

    LOGI("Starting hcid deamon");
    if (property_set("ctl.start", "hcid") < 0) {
        LOGE("Failed to start hcid");
        goto out;
    }
    sleep(HCID_START_DELAY_SEC);

    ret = 0;

out:
    if (hci_sock >= 0) close(hci_sock);
    return ret;
}

int bt_disable() {
    LOGV(__FUNCTION__);

    int ret = -1;
    int hci_sock = -1;

    LOGI("Stopping hcid deamon");
    if (property_set("ctl.stop", "hcid") < 0) {
        LOGE("Error stopping hcid");
        goto out;
    }
    usleep(HCID_STOP_DELAY_USEC);

    hci_sock = create_hci_sock();
    if (hci_sock < 0) goto out;
    ioctl(hci_sock, HCIDEVDOWN, HCI_DEV_ID);

    LOGI("Stopping hciattach deamon");
    if (property_set("ctl.stop", "hciattach") < 0) {
        LOGE("Error stopping hciattach");
        goto out;
    }

    if (set_bluetooth_power(0) < 0) {
        goto out;
    }
    ret = 0;

out:
    if (hci_sock >= 0) close(hci_sock);
    return ret;
}

int bt_is_enabled() {
    LOGV(__FUNCTION__);

    int hci_sock = -1;
    int ret = -1;
    struct hci_dev_info dev_info;


    // Check power first
    ret = check_bluetooth_power();
    if (ret == -1 || ret == 0) goto out;

    ret = -1;

    // Power is on, now check if the HCI interface is up
    hci_sock = create_hci_sock();
    if (hci_sock < 0) goto out;

    dev_info.dev_id = HCI_DEV_ID;
    if (ioctl(hci_sock, HCIGETDEVINFO, (void *)&dev_info) < 0) {
        ret = 0;
        goto out;
    }

    ret = hci_test_bit(HCI_UP, &dev_info.flags);

out:
    if (hci_sock >= 0) close(hci_sock);
    return ret;
}
