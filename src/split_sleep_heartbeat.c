/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_actping

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/pm.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static struct k_work_delayable heartbeat_work;

static void heartbeat_work_handler(struct k_work *work) {
    if (zmk_activity_get_state() == ZMK_ACTIVITY_SLEEP) {
        return;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_DRV_INST(0)),
    };
    struct zmk_behavior_binding_event event = {
        .timestamp = k_uptime_get(),
    };

    (void)zmk_behavior_invoke_binding(&binding, event, true);

    k_work_reschedule(&heartbeat_work, K_MSEC(CONFIG_ZMK_SPLIT_SLEEP_HEARTBEAT_INTERVAL_MS));
}

static int central_activity_listener_cb(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        k_work_cancel_delayable(&heartbeat_work);
    } else if (!k_work_delayable_is_pending(&heartbeat_work)) {
        k_work_reschedule(&heartbeat_work, K_MSEC(CONFIG_ZMK_SPLIT_SLEEP_HEARTBEAT_INTERVAL_MS));
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(split_sleep_heartbeat_central, central_activity_listener_cb);
ZMK_SUBSCRIPTION(split_sleep_heartbeat_central, zmk_activity_state_changed);

static int split_sleep_heartbeat_init(void) {
    k_work_init_delayable(&heartbeat_work, heartbeat_work_handler);
    k_work_reschedule(&heartbeat_work, K_MSEC(CONFIG_ZMK_SPLIT_SLEEP_HEARTBEAT_INTERVAL_MS));
    return 0;
}

SYS_INIT(split_sleep_heartbeat_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#else /* !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

extern struct k_timer activity_timer;
extern bool is_usb_power_present(void);
extern int set_state(enum zmk_activity_state state);

static int64_t last_local_activity;
static int64_t last_split_heartbeat;

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    last_split_heartbeat = k_uptime_get();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int peripheral_local_activity_listener_cb(const zmk_event_t *eh) {
    last_local_activity = k_uptime_get();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(split_sleep_heartbeat_peripheral, peripheral_local_activity_listener_cb);
ZMK_SUBSCRIPTION(split_sleep_heartbeat_peripheral, zmk_position_state_changed);
ZMK_SUBSCRIPTION(split_sleep_heartbeat_peripheral, zmk_sensor_event);

static void peripheral_sleep_work_handler(struct k_work *work) {
    k_timer_stop(&activity_timer);

    int64_t now = k_uptime_get();
    int64_t local_inactive_time = now - last_local_activity;
    int64_t sleep_last_uptime = MAX(last_local_activity, last_split_heartbeat);
    int64_t sleep_inactive_time = now - sleep_last_uptime;

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (sleep_inactive_time > CONFIG_ZMK_IDLE_SLEEP_TIMEOUT && !is_usb_power_present()) {
        set_state(ZMK_ACTIVITY_SLEEP);

        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("Failed to suspend all the devices");
            zmk_pm_resume_devices();
            return;
        }

        sys_poweroff();
        return;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_SLEEP) */

    if (local_inactive_time > CONFIG_ZMK_IDLE_TIMEOUT) {
        set_state(ZMK_ACTIVITY_IDLE);
    }
}

K_WORK_DEFINE(peripheral_sleep_work, peripheral_sleep_work_handler);

static void peripheral_sleep_timer_expiry(struct k_timer *_timer) {
    k_work_submit(&peripheral_sleep_work);
}

K_TIMER_DEFINE(peripheral_sleep_timer, peripheral_sleep_timer_expiry, NULL);

static int split_sleep_heartbeat_init(void) {
    int64_t now = k_uptime_get();
    last_local_activity = now;
    last_split_heartbeat = now;

    k_timer_stop(&activity_timer);
    k_timer_start(&peripheral_sleep_timer, K_SECONDS(1), K_SECONDS(1));
    return 0;
}

// Must run after activity_init (CONFIG_APPLICATION_INIT_PRIORITY, default 90) so we stop
// ZMK's stock activity_timer after it has been started.
BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < 95,
             "split_sleep_heartbeat_init must run after activity_init");
SYS_INIT(split_sleep_heartbeat_init, APPLICATION, 95);

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_actping_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_actping_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
