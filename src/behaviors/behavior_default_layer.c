/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_default_layer

#include <drivers/behavior.h>
#include <dt-bindings/zmk_behavior_default_layer/default_layer.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// Save quickly (100 ms) rather than waiting for CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE (60 s),
// so power-cycling shortly after switching Win/Mac still persists the selection.
#define DEFAULT_LAYER_SAVE_DELAY_MS 100

struct default_layer_settings_t {
    uint8_t endpoint_defaults[ZMK_ENDPOINT_COUNT];
};

static struct default_layer_settings_t default_layers = {0};

#if IS_ENABLED(CONFIG_SETTINGS)
static struct k_work_delayable df_layers_save_work;
#endif

// Keyed by the selected output endpoint, so USB has its own default layer. A charge-only
// connection never becomes the selected endpoint, so it keeps the BLE profile's layer.
static struct zmk_endpoint_instance get_current_endpoint(void) { return zmk_endpoints_selected(); }

static zmk_keymap_layer_index_t zmk_default_layer_get(struct zmk_endpoint_instance endpoint) {
    uint8_t index = zmk_endpoint_instance_to_index(endpoint);
    if (index >= ZMK_ENDPOINT_COUNT) {
        return CONFIG_ZMK_DEFAULT_LAYER_MIN_INDEX;
    }
    return default_layers.endpoint_defaults[index];
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void zmk_default_layers_save_state_work(struct k_work *_work) {
    int ret = settings_save_one("default_layer/settings", &default_layers, sizeof(default_layers));
    if (ret < 0 && ret != -ENOENT) {
        LOG_ERR("default-layer save-failed rc=%d", ret);
    }
}
#endif

static int apply_default_layer_config(struct zmk_endpoint_instance endpoint) {
    zmk_keymap_layer_index_t layer_index = zmk_default_layer_get(endpoint);
    zmk_keymap_layer_id_t target_layer_id = zmk_keymap_layer_index_to_id(layer_index);
    zmk_keymap_layer_id_t global_default_layer_id = zmk_keymap_layer_default();

    for (int i = CONFIG_ZMK_DEFAULT_LAYER_MIN_INDEX; i <= CONFIG_ZMK_DEFAULT_LAYER_MAX_INDEX; i++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(i);
        if (layer_id != target_layer_id && layer_id != global_default_layer_id) {
            int rc = zmk_keymap_layer_deactivate(layer_id);
            if (rc != 0) {
                LOG_WRN("default-layer deactivate-failed layer=%d rc=%d", i, rc);
            }
        }
    }

    if (target_layer_id != global_default_layer_id) {
        int ret = zmk_keymap_layer_activate(target_layer_id);
        if (ret < 0) {
            LOG_WRN("default-layer activate-failed layer=%d rc=%d", layer_index, ret);
            return ret;
        }
    }

    return 0;
}

static void apply_current_endpoint_work_cb(struct k_work *_work) {
    apply_default_layer_config(get_current_endpoint());
}

K_WORK_DEFINE(apply_current_endpoint_work, apply_current_endpoint_work_cb);

#if IS_ENABLED(CONFIG_SETTINGS)

static int default_layer_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "settings", &next) && !next) {
        if (len != sizeof(default_layers)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &default_layers, sizeof(default_layers));
        return (rc >= 0) ? 0 : rc;
    }

    return -ENOENT;
}

static int default_layer_handle_commit(void) {
    k_work_submit(&apply_current_endpoint_work);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(default_layer, "default_layer", NULL, default_layer_handle_set,
                               default_layer_handle_commit, NULL);

#endif

static int default_layer_init(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&df_layers_save_work, zmk_default_layers_save_state_work);
#endif
    return apply_default_layer_config(get_current_endpoint());
}

SYS_INIT(default_layer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int zmk_default_layer_set(struct zmk_endpoint_instance endpoint,
                                 zmk_keymap_layer_index_t layer) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return -EINVAL;
    }
    if (layer < CONFIG_ZMK_DEFAULT_LAYER_MIN_INDEX || CONFIG_ZMK_DEFAULT_LAYER_MAX_INDEX < layer) {
        return -EINVAL;
    }

    uint8_t index = zmk_endpoint_instance_to_index(endpoint);
    if (index >= ZMK_ENDPOINT_COUNT) {
        return -EINVAL;
    }

    bool changed = (default_layers.endpoint_defaults[index] != layer);
    default_layers.endpoint_defaults[index] = layer;

    int ret = apply_default_layer_config(endpoint);
    if (ret < 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    if (changed) {
        ret = k_work_reschedule(&df_layers_save_work, K_MSEC(DEFAULT_LAYER_SAVE_DELAY_MS));
        return MIN(0, ret);
    }
#endif
    return 0;
}

static int behavior_default_layer_init(const struct device *dev) { return 0; }

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    struct zmk_endpoint_instance endpoint = get_current_endpoint();

    switch (binding->param1) {
    case DEFAULT_LAYER_CMD_SELECT:
        return zmk_default_layer_set(endpoint, binding->param2);
    case DEFAULT_LAYER_CMD_NEXT: {
        zmk_keymap_layer_index_t current = zmk_default_layer_get(endpoint);
        zmk_keymap_layer_index_t next = current >= CONFIG_ZMK_DEFAULT_LAYER_MAX_INDEX
                                            ? CONFIG_ZMK_DEFAULT_LAYER_MIN_INDEX
                                            : current + 1;
        return zmk_default_layer_set(endpoint, next);
    }
    default:
        LOG_ERR("Unknown command for df: %d", binding->param1);
    }
    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata select_param1_values[] = {
    {
        .display_name = "Select default layer for current transport",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DEFAULT_LAYER_CMD_SELECT,
    },
};

static const struct behavior_parameter_value_metadata select_param2_values[] = {
    {
        .display_name = "Layer index",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = CONFIG_ZMK_DEFAULT_LAYER_MIN_INDEX,
                .max = CONFIG_ZMK_DEFAULT_LAYER_MAX_INDEX,
            },
    },
};

static const struct behavior_parameter_metadata_set select_metadata_set = {
    .param1_values = select_param1_values,
    .param1_values_len = ARRAY_SIZE(select_param1_values),
    .param2_values = select_param2_values,
    .param2_values_len = ARRAY_SIZE(select_param2_values),
};

static const struct behavior_parameter_value_metadata next_param1_values[] = {
    {
        .display_name = "Select next default layer for current transport",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DEFAULT_LAYER_CMD_NEXT,
    },
};

static const struct behavior_parameter_metadata_set next_metadata_set = {
    .param1_values = next_param1_values,
    .param1_values_len = ARRAY_SIZE(next_param1_values),
};

static const struct behavior_parameter_metadata_set metadata_sets[] = {select_metadata_set,
                                                                       next_metadata_set};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(metadata_sets),
    .sets = metadata_sets,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_default_layer_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_default_layer_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_default_layer_driver_api);

static int endpoint_changed_cb(const zmk_event_t *eh) {
    k_work_submit(&apply_current_endpoint_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(default_layer, endpoint_changed_cb);
ZMK_SUBSCRIPTION(default_layer, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(default_layer, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(default_layer, zmk_ble_active_profile_changed);
#endif

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
