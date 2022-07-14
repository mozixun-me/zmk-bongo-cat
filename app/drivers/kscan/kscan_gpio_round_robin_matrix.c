/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "debounce.h"

#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <drivers/kscan.h>
#include <kernel.h>
#include <logging/log.h>
#include <sys/__assert.h>
#include <sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_kscan_gpio_round_robin_matrix

#define INST_INPUT_LEN(n) DT_INST_PROP_LEN(n, input_gpios)
#define INST_OUTPUTS_LEN(n) DT_INST_PROP_LEN(n, output_gpios)
#define INST_MATRIX_LEN(n) (INST_INPUT_LEN(n) * INST_OUTPUTS_LEN(n))

#if CONFIG_ZMK_KSCAN_DEBOUNCE_PRESS_MS >= 0
#define INST_DEBOUNCE_PRESS_MS(n) CONFIG_ZMK_KSCAN_DEBOUNCE_PRESS_MS
#else
#define INST_DEBOUNCE_PRESS_MS(n)                                                                  \
    DT_INST_PROP_OR(n, debounce_period, DT_INST_PROP(n, debounce_press_ms))
#endif

#if CONFIG_ZMK_KSCAN_DEBOUNCE_RELEASE_MS >= 0
#define INST_DEBOUNCE_RELEASE_MS(n) CONFIG_ZMK_KSCAN_DEBOUNCE_RELEASE_MS
#else
#define INST_DEBOUNCE_RELEASE_MS(n)                                                                \
    DT_INST_PROP_OR(n, debounce_period, DT_INST_PROP(n, debounce_release_ms))
#endif

#define KSCAN_GPIO_INPUT_CFG_INIT(idx, inst_idx)                                                   \
    GPIO_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst_idx), input_gpios, idx),
#define KSCAN_GPIO_OUTPUT_CFG_INIT(idx, inst_idx)                                                  \
    GPIO_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst_idx), output_gpios, idx),

struct kscan_round_robin_matrix_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    int64_t scan_time;
    struct debounce_state *matrix_state;
};

struct kscan_gpio_list {
    const struct gpio_dt_spec *gpios;
    size_t len;
};

#define KSCAN_GPIO_LIST(gpio_array)                                                                \
    ((struct kscan_gpio_list){.gpios = gpio_array, .len = ARRAY_SIZE(gpio_array)})

struct kscan_round_robin_matrix_config {
    struct kscan_gpio_list outputs;
    struct kscan_gpio_list inputs;
    struct debounce_config debounce_config;
    int32_t poll_period_us;
};

static int state_index_io(const struct kscan_round_robin_matrix *config, const int input_idx,
                          const int output_idx) {
    __ASSERT(input_idx < config->inputs.len, "Invalid input %i", input_idx);
    __ASSERT(output_idx < config->outputs.len, "Invalid output %i", output_idx);

    return (input_idx * config->outputs.len) + output_idx);
}    

static int kscan_round_robin_matrix_read(const struct device *dev) {
    struct kscan_round_robin_matrix_data *data = dev->data;
    const struct kscan_round_robin_matrix_config *config = dev->config;

    size_t len = config->outputs.len;
    for (int o = 0; o < len; o++) {
        const struct gpio_dt_spec *out_gpio = &config->outputs.gpios[o];

        int err = gpio_pin_set_dt(out_gpio, 1);
        if (err) {
            LOG_ERR("Failed to set output %i active: %i", o, err);
            return err;
        }

        for (int i = (o + 1) % len; i != o; i = (i + 1) % len) {
            const struct gpio_dt_spec *in_gpio = &config->inputs.gpios[i];

            const int index = state_index_io(config, i, o);
            const bool is_active = gpio_pin_get_dt(in_gpio);
            struct debounce_state *state = &data->matrix_state[index];

            debounce_update(state, is_active, config->debounce_scan_period_ms,
                            &config->debounce_config);

            if (debounce_get_changed(state)) {
                const bool is_pressed = debounce_is_pressed(state);

                LOG_DBG("Sending event at %i,%i state %s", r, c, is_pressed ? "on" : "off");
                data->callback(dev, i, o, is_pressed);
            }
        }

        err = gpio_pin_set_dt(out_gpio, 0);
        if (err) {
            LOG_ERR("Failed to set output %i inactive: %i", o, err);
            return err;
        }
    }

    data->scan_time += config->poll_period_ms;

    k_work_reschedule(&data->work, K_TIMEOUT_ABS_MS(data->scan_time));

    return 0;
}

static void kscan_matrix_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_round_robin_matrix_data *data = CONTAINER_OF(dwork, struct kscan_round_robin_matrix_data, work);
    kscan_round_robin_matrix_read(data->dev);
}

static int kscan_round_robin_matrix_init(const struct device *dev) {
    struct kscan_round_robin_matrix_data *data = dev->data;

    data->dev = dev;

    k_work_init_delayable(&data->work, kscan_round_robin_matrix_work_handler);

    return 0;
}

static const struct kscan_driver_api kscan_round_robin_matrix_api = {
    .config = kscan_round_robin_matrix_configure,
    .enable_callback = kscan_round_robin_matrix_enable,
    .disable_callback = kscan_round_robin_matrix_disable,
};

#define KSCAN_ROUND_ROBIN_MATRIX_INIT(n)                                                           \
    BUILD_ASSERT(INST_DEBOUNCE_PRESS_MS(n) <= DEBOUNCE_COUNTER_MAX,                                \
                 "ZMK_KSCAN_DEBOUNCE_PRESS_MS or debounce-press-ms is too large");                 \
    BUILD_ASSERT(INST_DEBOUNCE_RELEASE_MS(n) <= DEBOUNCE_COUNTER_MAX,                              \
                 "ZMK_KSCAN_DEBOUNCE_RELEASE_MS or debounce-release-ms is too large");             \
                                                                                                   \
    static const struct gpio_dt_spec kscan_round_robin_matrix_outputs_##n[] = {                    \
        UTIL_LISTIFY(INST_OUTPUTS_LEN(n), KSCAN_GPIO_OUTPUT_CFG_INIT, n)};                         \
                                                                                                   \
    static const struct gpio_dt_spec kscan_round_robin_matrix_inputs_##n[] = {                     \
        UTIL_LISTIFY(INST_INPUTS_LEN(n), KSCAN_GPIO_INPUT_CFG_INIT, n)};                           \
                                                                                                   \
    static struct debounce_state kscan_round_robin_matrix_state_##n[INST_MATRIX_LEN(n)];           \
    \
    static struct kscan_round_robin_matrix_data kscan_round_robin_matrix_data_##n = {      \
        .matrix_state = kscan_round_robin_matrix_state_##n, }; \
    \
    static struct kscan_round_robin_matrix_config kscan_round_robin_matrix_config_##n = { \
        .outputs = KSCAN_GPIO_LIST(kscan_round_robin_matrix_outputs_##n)                           \
        .inputs = KSCAN_GPIO_LIST(kscan_round_robin_matrix_inputs_##n)                             \
        .debounce_config =                                                                         \
            {                                                                                      \
                .debounce_press_ms = INST_DEBOUNCE_PRESS_MS(n),                                    \
                .debounce_release_ms = INST_DEBOUNCE_RELEASE_MS(n),                                \
            },                                                                                     \
        .poll_period_ms = DT_INST_PROP(n, poll_period_ms),                                         \
    }; \
    DEVICE_DT_INST_DEFINE(n, &kscan_round_robin_matrix_init, NULL, &kscan_round_robin_matrix_data_##n,                     \
                          &kscan_round_robin_matrix_config_##n, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY, \
                          &kscan_round_robin_matrix_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_ROUND_ROBIN_MATRIX_INIT);
