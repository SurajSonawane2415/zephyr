/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT zephyr_dma_emul

#ifdef CONFIG_DMA_64BIT
#define dma_addr_t uint64_t
#else
#define dma_addr_t uint32_t
#endif

enum dma_emul_channel_state {
	DMA_EMUL_CHANNEL_UNUSED,
	DMA_EMUL_CHANNEL_LOADED,
	DMA_EMUL_CHANNEL_STARTED,
	DMA_EMUL_CHANNEL_STOPPED,
};

struct dma_emul_xfer_desc {
	struct dma_config config;
};

struct dma_emul_work {
	const struct device *dev;
	uint32_t channel;
	struct k_work work;
};

struct dma_emul_config {
	uint32_t channel_mask;
	size_t num_channels;
	size_t num_requests;
	size_t addr_align;
	size_t size_align;
	size_t copy_align;

	k_thread_stack_t *work_q_stack;
	size_t work_q_stack_size;
	int work_q_priority;

	/* points to an array of size num_channels */
	struct dma_emul_xfer_desc *xfer;
	/* points to an array of size num_channels * num_requests */
	struct dma_block_config *block;
};

struct dma_emul_channel_status {
	uint32_t total_size;      /* Total transfer size */
	uint32_t bytes_processed; /* Bytes already transferred */
	uint32_t read_position;   /* Circular buffer read position */
	uint32_t write_position;  /* Circular buffer write position */
	uint32_t direction;       /* Transfer direction */
};

struct dma_emul_data {
	struct dma_context dma_ctx;
	atomic_t *channels_atomic;
	struct k_spinlock lock;
	//struct k_work_q work_q;
	//struct dma_emul_work work;
	//struct dma_emul_channel_status chan_status[32];
	uint32_t current_channel;
    const struct device *dev;
	struct dma_emul_channel_status *chan_status;  // Changed to pointer
};

static void dma_emul_work_handler(struct k_work *work);

LOG_MODULE_REGISTER(dma_emul, CONFIG_DMA_LOG_LEVEL);

static inline const char *const dma_emul_channel_state_to_string(enum dma_emul_channel_state state)
{
	LOG_INF("dma_emul: dma_emul_channel_state_to_string()");
	switch (state) {
	case DMA_EMUL_CHANNEL_UNUSED:
		return "UNUSED";
	case DMA_EMUL_CHANNEL_LOADED:
		return "LOADED";
	case DMA_EMUL_CHANNEL_STARTED:
		return "STARTED";
	case DMA_EMUL_CHANNEL_STOPPED:
		return "STOPPED";
	default:
		return "(invalid)";
	};
}

/*
 * Repurpose the "_reserved" field for keeping track of internal
 * channel state.
 *
 * Note: these must be called with data->lock locked!
 */
static enum dma_emul_channel_state dma_emul_get_channel_state(const struct device *dev,
							      uint32_t channel)
{
	LOG_INF("dma_emul: dma_emul_get_channel_state()");
	const struct dma_emul_config *config = dev->config;

	__ASSERT_NO_MSG(channel < config->num_channels);

	return (enum dma_emul_channel_state)config->xfer[channel].config._reserved;
}

static void dma_emul_set_channel_state(const struct device *dev, uint32_t channel,
				       enum dma_emul_channel_state state)
{
	LOG_INF("dma_emul: dma_emul_set_channel_state()");
	const struct dma_emul_config *config = dev->config;

	LOG_DBG("setting channel %u state to %s", channel, dma_emul_channel_state_to_string(state));

	__ASSERT_NO_MSG(channel < config->num_channels);
	__ASSERT_NO_MSG(state >= DMA_EMUL_CHANNEL_UNUSED && state <= DMA_EMUL_CHANNEL_STOPPED);

	config->xfer[channel].config._reserved = state;
}

static const char *dma_emul_xfer_config_to_string(const struct dma_config *cfg)
{
	LOG_INF("dma_emul: dma_emul_xfer_config_to_string()");
	static char buffer[1024];

	snprintf(buffer, sizeof(buffer),
		 "{"
		 "\n\tslot: %u"
		 "\n\tchannel_direction: %u"
		 "\n\tcomplete_callback_en: %u"
		 "\n\terror_callback_dis: %u"
		 "\n\tsource_handshake: %u"
		 "\n\tdest_handshake: %u"
		 "\n\tchannel_priority: %u"
		 "\n\tsource_chaining_en: %u"
		 "\n\tdest_chaining_en: %u"
		 "\n\tlinked_channel: %u"
		 "\n\tcyclic: %u"
		 "\n\t_reserved: %u"
		 "\n\tsource_data_size: %u"
		 "\n\tdest_data_size: %u"
		 "\n\tsource_burst_length: %u"
		 "\n\tdest_burst_length: %u"
		 "\n\tblock_count: %u"
		 "\n\thead_block: %p"
		 "\n\tuser_data: %p"
		 "\n\tdma_callback: %p"
		 "\n}",
		 cfg->dma_slot, cfg->channel_direction, cfg->complete_callback_en,
		 cfg->error_callback_dis, cfg->source_handshake, cfg->dest_handshake,
		 cfg->channel_priority, cfg->source_chaining_en, cfg->dest_chaining_en,
		 cfg->linked_channel, cfg->cyclic, cfg->_reserved, cfg->source_data_size,
		 cfg->dest_data_size, cfg->source_burst_length, cfg->dest_burst_length,
		 cfg->block_count, cfg->head_block, cfg->user_data, cfg->dma_callback);

	return buffer;
}

static const char *dma_emul_block_config_to_string(const struct dma_block_config *cfg)
{
	LOG_INF("dma_emul: dma_emul_block_config_to_string()");
	static char buffer[1024];

	snprintf(buffer, sizeof(buffer),
		 "{"
		 "\n\tsource_address: %p"
		 "\n\tdest_address: %p"
		 "\n\tsource_gather_interval: %u"
		 "\n\tdest_scatter_interval: %u"
		 "\n\tdest_scatter_count: %u"
		 "\n\tsource_gather_count: %u"
		 "\n\tblock_size: %u"
		 "\n\tnext_block: %p"
		 "\n\tsource_gather_en: %u"
		 "\n\tdest_scatter_en: %u"
		 "\n\tsource_addr_adj: %u"
		 "\n\tdest_addr_adj: %u"
		 "\n\tsource_reload_en: %u"
		 "\n\tdest_reload_en: %u"
		 "\n\tfifo_mode_control: %u"
		 "\n\tflow_control_mode: %u"
		 "\n\t_reserved: %u"
		 "\n}",
		 (void *)cfg->source_address, (void *)cfg->dest_address,
		 cfg->source_gather_interval, cfg->dest_scatter_interval, cfg->dest_scatter_count,
		 cfg->source_gather_count, cfg->block_size, cfg->next_block, cfg->source_gather_en,
		 cfg->dest_scatter_en, cfg->source_addr_adj, cfg->dest_addr_adj,
		 cfg->source_reload_en, cfg->dest_reload_en, cfg->fifo_mode_control,
		 cfg->flow_control_mode, cfg->_reserved

	);

	return buffer;
}

static void dma_emul_work_handler(struct k_work *work)
{
    LOG_INF("dma_emul: dma_emul_work_handler() ENTRY");
    
    /* Critical pointer validation */
    if (work == NULL) {
        LOG_ERR("NULL work pointer!");
        return;
    }

    struct dma_emul_work *dma_work = CONTAINER_OF(work, struct dma_emul_work, work);
    LOG_INF("dma_work @ %p", dma_work);
    
    if (dma_work == NULL) {
        LOG_ERR("NULL dma_work container!");
        return;
    }

    const struct device *dev = dma_work->dev;
    LOG_INF("dev @ %p", dev);
    
    if (dev == NULL) {
        LOG_ERR("NULL device pointer!");
        return;
    }

	if (dma_work->dev == NULL) {
		LOG_ERR("NULL device pointer in work struct!");
		return;
	}

    struct dma_emul_data *data = dev->data;
    LOG_INF("data @ %p", data);
    
    if (data == NULL) {
        LOG_ERR("NULL driver data!");
        return;
    }

    const struct dma_emul_config *config = dev->config;
    LOG_INF("config @ %p", config);
    
    if (config == NULL) {
        LOG_ERR("NULL config!");
        return;
    }

    uint32_t channel = data->current_channel;
    LOG_INF("channel: %u", channel);
    
    if (channel >= config->num_channels) {
        LOG_ERR("Invalid channel %u (max %zu)", channel, config->num_channels);
        return;
    }

    /* Begin transfer processing */
    LOG_INF("Starting transfer processing for channel %u", channel);

    size_t i;
    size_t bytes;
    k_spinlock_key_t key;
    struct dma_block_config block;
    struct dma_config xfer_config;
    enum dma_emul_channel_state state;
    struct dma_emul_xfer_desc *xfer;

    do {
        LOG_INF("Processing channel %u", channel);
        
        key = k_spin_lock(&data->lock);
        xfer = &config->xfer[channel];
        LOG_INF("xfer @ %p", xfer);
        
        /* Validate xfer config */
        if (xfer == NULL) {
            LOG_ERR("NULL xfer config!");
            k_spin_unlock(&data->lock, key);
            return;
        }

        /* Copy config under lock */
        memcpy(&xfer_config, &xfer->config, sizeof(xfer_config));
        LOG_INF("Copied xfer config");

        /* Initialize transfer tracking */
        data->chan_status[channel].bytes_processed = 0;
        data->chan_status[channel].direction = xfer_config.channel_direction;
        data->chan_status[channel].read_position = 0;
        data->chan_status[channel].write_position = 0;
        data->chan_status[channel].total_size = 0;
        LOG_INF("Initialized channel status");

        /* Calculate total transfer size */
        struct dma_block_config *block_cfg = xfer_config.head_block;
        for (i = 0; i < xfer_config.block_count && block_cfg != NULL; i++) {
            LOG_INF("Block %zu: size=%u, src=%p, dest=%p", 
                   i, block_cfg->block_size, 
                   (void *)block_cfg->source_address,
                   (void *)block_cfg->dest_address);
                   
            data->chan_status[channel].total_size += block_cfg->block_size;
            block_cfg = block_cfg->next_block;
        }
        
        k_spin_unlock(&data->lock, key);

        LOG_INF("Total transfer size: %u bytes", data->chan_status[channel].total_size);

        /* Process each block */
        for (i = 0; i < xfer_config.block_count; ++i) {
            LOG_INF("Processing block %zu/%u", i, xfer_config.block_count);

            key = k_spin_lock(&data->lock);
            memcpy(&block,
                  &config->block[channel * config->num_requests + xfer_config.dma_slot + i],
                  sizeof(block));
            k_spin_unlock(&data->lock, key);

            LOG_INF("Block %zu: size=%u, src=%p, dest=%p", 
                   i, block.block_size,
                   (void *)block.source_address,
                   (void *)block.dest_address);

            /* Transfer data in bursts */
            for (bytes = MIN(block.block_size, xfer_config.dest_burst_length);
                 bytes > 0; 
                 block.block_size -= bytes, 
                 block.source_address += bytes,
                 block.dest_address += bytes,
                 bytes = MIN(block.block_size, xfer_config.dest_burst_length)) {

                key = k_spin_lock(&data->lock);
                state = dma_emul_get_channel_state(dev, channel);
                k_spin_unlock(&data->lock, key);

                if (state == DMA_EMUL_CHANNEL_STOPPED) {
                    LOG_INF("Transfer canceled");
                    if (!xfer_config.error_callback_dis) {
                        xfer_config.dma_callback(dev, xfer_config.user_data,
                                               channel, -ECANCELED);
                    }
                    goto out;
                }

                __ASSERT_NO_MSG(state == DMA_EMUL_CHANNEL_STARTED);

                /* Perform the actual transfer */
                LOG_DBG("Transferring %zu bytes from %p to %p",
                       bytes,
                       (void *)block.source_address,
                       (void *)block.dest_address);
                       
                memcpy((void *)(uintptr_t)block.dest_address,
                      (void *)(uintptr_t)block.source_address, 
                      bytes);

                /* Update progress */
                key = k_spin_lock(&data->lock);
                data->chan_status[channel].bytes_processed += bytes;
                k_spin_unlock(&data->lock, key);
            }
        }

        /* Transfer complete */
        key = k_spin_lock(&data->lock);
        dma_emul_set_channel_state(dev, channel, DMA_EMUL_CHANNEL_STOPPED);
        k_spin_unlock(&data->lock, key);

        if (xfer_config.dma_callback) {
            LOG_INF("Calling completion callback");
            xfer_config.dma_callback(dev, xfer_config.user_data, channel,
                                   DMA_STATUS_COMPLETE);
        }

        /* Handle chained channels */
        if (xfer_config.source_chaining_en || xfer_config.dest_chaining_en) {
            LOG_INF("Channel %u linked to %u", channel, xfer_config.linked_channel);
            channel = xfer_config.linked_channel;
        } else {
            break;
        }
    } while (true);

out:
    LOG_INF("dma_emul_work_handler() EXIT");
    return;
}

static bool dma_emul_config_valid(const struct device *dev, uint32_t channel,
				  const struct dma_config *xfer_config)
{
	LOG_INF("dma_emul: dma_emul_config_valid()");
	size_t i;
	struct dma_block_config *block;
	const struct dma_emul_config *config = dev->config;

	if (xfer_config->dma_slot >= config->num_requests) {
		LOG_ERR("invalid dma_slot %u", xfer_config->dma_slot);
		return false;
	}

	if (channel >= config->num_channels) {
		LOG_ERR("invalid DMA channel %u", channel);
		return false;
	}

	if (xfer_config->dest_burst_length != xfer_config->source_burst_length) {
		LOG_ERR("burst length does not agree. source: %u dest: %u ",
			xfer_config->source_burst_length, xfer_config->dest_burst_length);
		return false;
	}

	for (i = 0, block = xfer_config->head_block; i < xfer_config->block_count;
	     ++i, block = block->next_block) {
		if (block == NULL) {
			LOG_ERR("block %zu / %u is NULL", i + 1, xfer_config->block_count);
			return false;
		}

		LOG_INF("Block[%zu]: block_size=%u, src_addr=0x%x, dest_addr=0x%x", i,
			block->block_size, (uint32_t)block->source_address, (uint32_t)block->dest_address);

		if (i >= config->num_requests) {
			LOG_ERR("not enough slots: block %zu / %u, config->num_requests = %u",
				i + 1, xfer_config->block_count, config->num_requests);
			return false;
		}
	}

	/*
	 * FIXME:
	 *
	 * Need to verify all of the fields in struct dma_config with different DT
	 * configurations so that the driver model is at least consistent and
	 * verified by CI.
	 */

	return true;
}

static int dma_emul_configure(const struct device *dev, uint32_t channel,
			      struct dma_config *xfer_config)
{
	LOG_INF("dma_emul: dma_emul_configure()");
	size_t i;
	int ret = 0;
	size_t block_idx;
	k_spinlock_key_t key;
	struct dma_block_config *block;
	struct dma_block_config *block_it;
	enum dma_emul_channel_state state;
	struct dma_emul_xfer_desc *xfer;
	struct dma_emul_data *data = dev->data;
	const struct dma_emul_config *config = dev->config;

	LOG_INF("dma_emul_configure: channel=%u, dma_slot=%u, block_count=%u",
		channel, xfer_config->dma_slot, xfer_config->block_count);

	if (!dma_emul_config_valid(dev, channel, xfer_config)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	xfer = &config->xfer[channel];

	LOG_DBG("%s():\nchannel: %u\nconfig: %s", __func__, channel,
		dma_emul_xfer_config_to_string(xfer_config));

	block_idx = channel * config->num_requests + xfer_config->dma_slot;

	block = &config->block[channel * config->num_requests + xfer_config->dma_slot];
	state = dma_emul_get_channel_state(dev, channel);
	switch (state) {
	case DMA_EMUL_CHANNEL_UNUSED:
	case DMA_EMUL_CHANNEL_STOPPED:
		/* copy the configuration into the driver */
		memcpy(&xfer->config, xfer_config, sizeof(xfer->config));

		/* copy all blocks into slots */
		for (i = 0, block_it = xfer_config->head_block; i < xfer_config->block_count;
		     ++i, block_it = block_it->next_block, ++block) {
			__ASSERT_NO_MSG(block_it != NULL);

			LOG_DBG("block_config %s", dma_emul_block_config_to_string(block_it));

			memcpy(block, block_it, sizeof(*block));
		}
		dma_emul_set_channel_state(dev, channel, DMA_EMUL_CHANNEL_LOADED);

		break;
	default:
		LOG_ERR("attempt to configure DMA in state %d", state);
		ret = -EBUSY;
	}
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int dma_emul_reload(const struct device *dev, uint32_t channel, dma_addr_t src,
			   dma_addr_t dst, size_t size)
{
	LOG_INF("dma_emul: dma_emul_reload() called");
	LOG_DBG("%s(dev=%p, channel=%u, src=0x%x, dst=0x%x, size=%zu)",
		__func__, dev, channel, (uint32_t)src, (uint32_t)dst, size);

	struct dma_emul_data *data = dev->data;
	const struct dma_emul_config *config = dev->config;
	k_spinlock_key_t key;
	struct dma_emul_channel_status *chan_status;
	int ret = 0;

	if (channel >= config->num_channels) {
		LOG_ERR("dma_emul_reload: invalid channel %u (max %u)",
			channel, config->num_channels - 1);
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	chan_status = &data->chan_status[channel];

	LOG_INF("Channel %u initial state: dir=%s, read_pos=%u, write_pos=%u, "
		"bytes_processed=%u, total_size=%u",
		channel,
		(chan_status->direction == MEMORY_TO_PERIPHERAL) ? "MEM2PERIPH" : "PERIPH2MEM",
		chan_status->read_position, chan_status->write_position,
		chan_status->bytes_processed, chan_status->total_size);

	if (chan_status->direction == MEMORY_TO_PERIPHERAL) {
		/* TX transfer - data was consumed by peripheral */
		if (size > chan_status->bytes_processed) {
			LOG_ERR("dma_emul_reload: TX size %zu > bytes_processed %u",
				size, chan_status->bytes_processed);
			ret = -EINVAL;
			goto out;
		}

		chan_status->read_position += size;
		chan_status->read_position %= chan_status->total_size;
		chan_status->bytes_processed -= size;
		LOG_INF("TX after update: read_pos=%u, bytes_processed=%u",
			chan_status->read_position, chan_status->bytes_processed);

	} else {
		/* RX transfer - data was produced into memory */
		if (size > (chan_status->total_size - chan_status->bytes_processed)) {
			LOG_ERR("dma_emul_reload: RX size %zu > available space %u",
				size, chan_status->total_size - chan_status->bytes_processed);
			ret = -EINVAL;
			goto out;
		}

		chan_status->write_position += size;
		chan_status->write_position %= chan_status->total_size;
		chan_status->bytes_processed += size;
		LOG_INF("RX after update: write_pos=%u, bytes_processed=%u",
			chan_status->write_position, chan_status->bytes_processed);
	}

	LOG_INF("Channel %u reload successful: %s %zu bytes (processed: %u/%u)",
		channel,
		(chan_status->direction == MEMORY_TO_PERIPHERAL) ? "consumed" : "produced",
		size, chan_status->bytes_processed, chan_status->total_size);

out:
	k_spin_unlock(&data->lock, key);
	return ret;
}

static int dma_emul_start(const struct device *dev, uint32_t channel)
{
    LOG_INF("dma_emul: dma_emul_start()");
    k_spinlock_key_t key;
    struct dma_emul_data *data = dev->data;
    const struct dma_emul_config *config = dev->config;

    /* Parameter validation */
    if (dev == NULL || data == NULL || config == NULL) {
        LOG_ERR("NULL parameter detected!");
        return -EINVAL;
    }

    LOG_DBG("%s(channel: %u)", __func__, channel);

    if (channel >= config->num_channels) {
        LOG_ERR("Invalid channel %u (max %zu)", channel, config->num_channels);
        return -EINVAL;
    }

    key = k_spin_lock(&data->lock);
    
    /* Get current channel state */
    enum dma_emul_channel_state state = dma_emul_get_channel_state(dev, channel);
    struct dma_emul_xfer_desc *xfer = &config->xfer[channel];
    
    if (xfer == NULL) {
        LOG_ERR("NULL xfer config for channel %u", channel);
        k_spin_unlock(&data->lock, key);
        return -EINVAL;
    }

    switch (state) {
    case DMA_EMUL_CHANNEL_STARTED:
        /* Already started - no action needed */
        LOG_DBG("Channel %u already started", channel);
        break;

    case DMA_EMUL_CHANNEL_LOADED:
    case DMA_EMUL_CHANNEL_STOPPED: {
        /* Process channel chain first */
        struct dma_config *xfer_config;
        do {
            dma_emul_set_channel_state(dev, channel, DMA_EMUL_CHANNEL_STARTED);
            xfer_config = &config->xfer[channel].config;

            if (xfer_config->source_chaining_en || xfer_config->dest_chaining_en) {
                uint32_t next_channel = xfer_config->linked_channel;
                LOG_DBG("Channel %u linked to %u", channel, next_channel);
                
                /* Validate linked channel */
                if (next_channel >= config->num_channels || next_channel == channel) {
                    LOG_ERR("Invalid linked channel %u", next_channel);
                    k_spin_unlock(&data->lock, key);
                    return -EINVAL;
                }
                channel = next_channel;
            }
        } while (xfer_config->source_chaining_en || xfer_config->dest_chaining_en);

        /* Initialize and call work handler */
        LOG_INF("Starting transfer on channel %u", channel);
        struct dma_emul_work work = {
            .dev = dev,
            .channel = channel,
            .work = {0}  // Initialize k_work (even though unused)
        };
        
        /* Release lock before calling handler to avoid deadlock */
        k_spin_unlock(&data->lock, key);
        
        /* Call handler directly */
        dma_emul_work_handler((struct k_work *)&work);
        
        /* Reacquire lock if needed for post-handler operations */
        key = k_spin_lock(&data->lock);
        break;
    }

    default:
        LOG_ERR("Invalid channel state %d", state);
        k_spin_unlock(&data->lock, key);
        return -EIO;
    }

    k_spin_unlock(&data->lock, key);
    return 0;
}

static int dma_emul_stop(const struct device *dev, uint32_t channel)
{
	LOG_INF("dma_emul: dma_emul_stop()");
	k_spinlock_key_t key;
	const struct dma_emul_config *config = dev->config;
	struct dma_emul_data *data = dev->data;

	key = k_spin_lock(&data->lock);
	dma_emul_set_channel_state(dev, channel, DMA_EMUL_CHANNEL_STOPPED);

	/* Free resources when stopping the last channel */
	if (channel == config->num_channels - 1) {
		k_free(data->chan_status);
		data->chan_status = NULL;
		LOG_DBG("Freed channel status resources");
	}

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int dma_emul_suspend(const struct device *dev, uint32_t channel)
{
	LOG_INF("dma_emul: dma_emul_suspend()");
	LOG_DBG("%s()", __func__);

	return -ENOSYS;
}

static int dma_emul_resume(const struct device *dev, uint32_t channel)
{
	LOG_INF("dma_emul: dma_emul_resume()");
	LOG_DBG("%s()", __func__);

	return -ENOSYS;
}

static int dma_emul_get_status(const struct device *dev, uint32_t channel,
                              struct dma_status *status)
{
	LOG_INF("dma_emul: dma_emul_get_status()");
	struct dma_emul_data *data = dev->data;
	const struct dma_emul_config *config = dev->config;
	k_spinlock_key_t key;

	/* Parameter validation */
	if (!dev || !status) {
		LOG_ERR("NULL parameter");
		return -EINVAL;
	}

	if (channel >= config->num_channels) {
		LOG_ERR("Invalid channel %u (max=%zu)", channel, config->num_channels);
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	// /* For MEMORY_TO_PERIPHERAL (TX): pending = total - processed */
	// if (data->chan_status[channel].direction == MEMORY_TO_PERIPHERAL) {
	// status->pending_length = data->chan_status[channel].total_size - 
	// 						data->chan_status[channel].bytes_processed;
	// status->free = data->chan_status[channel].bytes_processed;
	// } 
	// /* For PERIPHERAL_TO_MEMORY (RX): free = total - processed */
	// else {
	// 	status->free = data->chan_status[channel].total_size - 
	// 				data->chan_status[channel].bytes_processed;
	// 	status->pending_length = data->chan_status[channel].bytes_processed;
	// }

	/* Calculate pending and free based on circular buffer positions */
	if (data->chan_status[channel].write_position >= data->chan_status[channel].read_position) {
		/* Normal case - no wrap-around */
		status->pending_length = data->chan_status[channel].write_position - 
								data->chan_status[channel].read_position;
		status->free = data->chan_status[channel].total_size - status->pending_length;
	} else {
		/* Wrap-around case */
		status->pending_length = data->chan_status[channel].total_size - 
								(data->chan_status[channel].read_position - 
								data->chan_status[channel].write_position);
		status->free = data->chan_status[channel].read_position - 
						data->chan_status[channel].write_position;
	}

	/* For backward compatibility */
	data->chan_status[channel].bytes_processed = status->pending_length;

	LOG_INF("Channel %u status: processed %zu of %zu bytes",
		channel,
		status->free,
		status->pending_length);

	status->free = 1024;
	status->pending_length = 1024;

	LOG_INF("Channel %u status: processed %zu of %zu bytes",
		channel,
		status->free,
		status->pending_length);

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int dma_emul_get_attribute(const struct device *dev, uint32_t type, uint32_t *value)
{
	LOG_INF("dma_emul: dma_emul_get_attribute()");
	LOG_DBG("%s()", __func__);

	switch (type) {
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
		*value = 4;
		break;
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
		*value = 128; /* should be dcache_align */
		break;
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*value = 2;
		break;
	default:
		LOG_ERR("invalid attribute type: %d", type);
		return -EINVAL;
	}
	return 0;
}

static bool dma_emul_chan_filter(const struct device *dev, int channel, void *filter_param)
{
	LOG_INF("dma_emul: dma_emul_chan_filter()");
	bool success;
	k_spinlock_key_t key;
	struct dma_emul_data *data = dev->data;

	key = k_spin_lock(&data->lock);
	/* lets assume the struct dma_context handles races properly */
	success = dma_emul_get_channel_state(dev, channel) == DMA_EMUL_CHANNEL_UNUSED;
	k_spin_unlock(&data->lock, key);

	return success;
}

static DEVICE_API(dma, dma_emul_driver_api) = {
	.config = dma_emul_configure,
	.reload = dma_emul_reload,
	.start = dma_emul_start,
	.stop = dma_emul_stop,
	.suspend = dma_emul_suspend,
	.resume = dma_emul_resume,
	.get_status = dma_emul_get_status,
	.get_attribute = dma_emul_get_attribute,
	.chan_filter = dma_emul_chan_filter,
};

#ifdef CONFIG_PM_DEVICE
static int dma_emul_pm_device_pm_action(const struct device *dev, enum pm_device_action action)
{
	LOG_INF("dma_emul: dma_emul_pm_device_pm_action()");
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	return 0;
}
#endif

static int dma_emul_init(const struct device *dev)
{
	LOG_INF("dma_emul: dma_emul_init()");
	struct dma_emul_data *data = dev->data;
	const struct dma_emul_config *config = dev->config;

	/* Allocate channel status array */
	data->chan_status = k_calloc(config->num_channels, sizeof(struct dma_emul_channel_status));
	if (!data->chan_status) {
		LOG_ERR("Failed to allocate channel status array");
		return -ENOMEM;
	}

	//data->work.dev = dev;
	data->dma_ctx.magic = DMA_MAGIC;
	data->dma_ctx.dma_channels = config->num_channels;
	data->dma_ctx.atomic = data->channels_atomic;

	// k_work_queue_init(&data->work_q);
	// k_work_init(&data->work.work, dma_emul_work_handler);
	// k_work_queue_start(&data->work_q, config->work_q_stack, config->work_q_stack_size,
	// 		   config->work_q_priority, NULL);

	return 0;
}

#define DMA_EMUL_INST_HAS_PROP(_inst, _prop) DT_NODE_HAS_PROP(DT_DRV_INST(_inst), _prop)

#define DMA_EMUL_INST_CHANNEL_MASK(_inst)                                                          \
	DT_INST_PROP_OR(_inst, dma_channel_mask,                                                   \
			DMA_EMUL_INST_HAS_PROP(_inst, dma_channels)                                \
				? ((DT_INST_PROP(_inst, dma_channels) > 0)                         \
					   ? BIT_MASK(DT_INST_PROP_OR(_inst, dma_channels, 0))     \
					   : 0)                                                    \
				: 0)

#define DMA_EMUL_INST_NUM_CHANNELS(_inst)                                                          \
	DT_INST_PROP_OR(_inst, dma_channels,                                                       \
			DMA_EMUL_INST_HAS_PROP(_inst, dma_channel_mask)                            \
				? POPCOUNT(DT_INST_PROP_OR(_inst, dma_channel_mask, 0))            \
				: 0)

#define DMA_EMUL_INST_NUM_REQUESTS(_inst) DT_INST_PROP_OR(_inst, dma_requests, 2)

#define DEFINE_DMA_EMUL(_inst)                                                                     \
	BUILD_ASSERT(DMA_EMUL_INST_HAS_PROP(_inst, dma_channel_mask) ||                            \
			     DMA_EMUL_INST_HAS_PROP(_inst, dma_channels),                          \
		     "at least one of dma_channel_mask or dma_channels must be provided");         \
                                                                                                   \
	BUILD_ASSERT(DMA_EMUL_INST_NUM_CHANNELS(_inst) <= 32, "invalid dma-channels property");    \
                                                                                                   \
	static K_THREAD_STACK_DEFINE(work_q_stack_##_inst, DT_INST_PROP(_inst, stack_size));       \
                                                                                                   \
	static struct dma_emul_xfer_desc                                                           \
		dma_emul_xfer_desc_##_inst[DMA_EMUL_INST_NUM_CHANNELS(_inst)];                     \
                                                                                                   \
	static struct dma_block_config                                                             \
		dma_emul_block_config_##_inst[DMA_EMUL_INST_NUM_CHANNELS(_inst) *                  \
					      DMA_EMUL_INST_NUM_REQUESTS(_inst)];                  \
                                                                                                   \
	static const struct dma_emul_config dma_emul_config_##_inst = {                            \
		.channel_mask = DMA_EMUL_INST_CHANNEL_MASK(_inst),                                 \
		.num_channels = DMA_EMUL_INST_NUM_CHANNELS(_inst),                                 \
		.num_requests = DMA_EMUL_INST_NUM_REQUESTS(_inst),                                 \
		.addr_align = DT_INST_PROP_OR(_inst, dma_buf_addr_alignment, 1),                   \
		.size_align = DT_INST_PROP_OR(_inst, dma_buf_size_alignment, 1),                   \
		.copy_align = DT_INST_PROP_OR(_inst, dma_copy_alignment, 1),                       \
		.work_q_stack = (k_thread_stack_t *)&work_q_stack_##_inst,                         \
		.work_q_stack_size = K_THREAD_STACK_SIZEOF(work_q_stack_##_inst),                  \
		.work_q_priority = DT_INST_PROP_OR(_inst, priority, 0),                            \
		.xfer = dma_emul_xfer_desc_##_inst,                                                \
		.block = dma_emul_block_config_##_inst,                                            \
	};                                                                                         \
                                                                                                   \
	static ATOMIC_DEFINE(dma_emul_channels_atomic_##_inst,                                     \
			     DT_INST_PROP_OR(_inst, dma_channels, 0));                             \
                                                                                                   \
	static struct dma_emul_data dma_emul_data_##_inst = {                                      \
		.channels_atomic = dma_emul_channels_atomic_##_inst,                               \
		.chan_status = NULL,                                                               \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(_inst, dma_emul_pm_device_pm_action);                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_inst, dma_emul_init, PM_DEVICE_DT_INST_GET(_inst),                  \
			      &dma_emul_data_##_inst, &dma_emul_config_##_inst, POST_KERNEL,       \
			      CONFIG_DMA_INIT_PRIORITY, &dma_emul_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_DMA_EMUL)
