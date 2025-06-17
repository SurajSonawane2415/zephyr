/*
 * Copyright (c) 2022 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/dai.h>
#include <zephyr/pm/device_runtime.h>
#include <string.h>

#define DT_DRV_COMPAT sof_virtual_dai
LOG_MODULE_REGISTER(virtual_dai, CONFIG_DAI_LOG_LEVEL);

struct virtual_dai_data {
	bool active;
	uint8_t direction;
};

static int virtual_dai_probe(const struct device *dev)
{
	LOG_INF("virtual_dai: probe()");
	return 0;
}

static int virtual_dai_remove(const struct device *dev)
{
	LOG_INF("virtual_dai: remove()");
	return 0;
}

static int virtual_dai_config_set(const struct device *dev, const struct dai_config *cfg)
{
	struct virtual_dai_data *data = dev->data;
	LOG_INF("virtual_dai: config_set() direction=%u", cfg->direction);
	data->direction = cfg->direction;
	return 0;
}

static int virtual_dai_config_get(const struct device *dev, struct dai_config *cfg)
{
	LOG_INF("virtual_dai: config_get()");
	return 0;
}

static int virtual_dai_get_properties(const struct device *dev, struct dai_properties *prop)
{
	LOG_INF("virtual_dai: get_properties()");
	prop->fifo_address = 0;
	prop->fifo_depth = 0;
	prop->stream_capabilities = DAI_STREAM_PLAYBACK | DAI_STREAM_CAPTURE;
	return 0;
}

static int virtual_dai_trigger(const struct device *dev, enum dai_dir dir, int cmd)
{
	struct virtual_dai_data *data = dev->data;

	switch (cmd) {
	case DAI_TRIGGER_START:
		data->active = true;
		LOG_INF("virtual_dai: START (dir=%d)", dir);
		break;
	case DAI_TRIGGER_STOP:
		data->active = false;
		LOG_INF("virtual_dai: STOP (dir=%d)", dir);
		break;
	case DAI_TRIGGER_PAUSE:
		LOG_INF("virtual_dai: PAUSE (dir=%d)", dir);
		break;
	case DAI_TRIGGER_RESUME:
		LOG_INF("virtual_dai: RESUME (dir=%d)", dir);
		break;
	case DAI_TRIGGER_PRE_START:
	case DAI_TRIGGER_PRE_RELEASE:
		LOG_INF("virtual_dai: PRE_START/RELEASE (dir=%d)", dir);
		break;
	default:
		LOG_WRN("virtual_dai: Unknown trigger %d", cmd);
		break;
	}
	return 0;
}

static int virtual_dai_ts_config(const struct device *dev, struct dai_ts_cfg *cfg)
{
	LOG_INF("virtual_dai: ts_config()");
	return 0;
}

static int virtual_dai_ts_start(const struct device *dev)
{
	LOG_INF("virtual_dai: ts_start()");
	return 0;
}

static int virtual_dai_ts_stop(const struct device *dev)
{
	LOG_INF("virtual_dai: ts_stop()");
	return 0;
}

static int virtual_dai_ts_get(const struct device *dev, struct dai_ts_data *tsd)
{
	ts_d->timestamp = k_uptime_get_32();
	LOG_INF("virtual_dai: ts_get() = %u", tsd->timestamp);
	return 0;
}

static const struct dai_driver_api virtual_dai_api = {
	.probe = virtual_dai_probe,
	.remove = virtual_dai_remove,
	.config_set = virtual_dai_config_set,
	.config_get = virtual_dai_config_get,
	.get_properties = virtual_dai_get_properties,
	.trigger = virtual_dai_trigger,
	.ts_config = virtual_dai_ts_config,
	.ts_start = virtual_dai_ts_start,
	.ts_stop = virtual_dai_ts_stop,
	.ts_get = virtual_dai_ts_get,
};

static struct virtual_dai_data virtual_dai_data_inst;

DEVICE_DT_INST_DEFINE(0, NULL, NULL,
			&virtual_dai_data_inst, NULL,
			POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
			&virtual_dai_api);
