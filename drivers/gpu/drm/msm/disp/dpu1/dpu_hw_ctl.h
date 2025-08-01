/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _DPU_HW_CTL_H
#define _DPU_HW_CTL_H

#include "dpu_hw_mdss.h"
#include "dpu_hw_util.h"
#include "dpu_hw_catalog.h"
#include "dpu_hw_sspp.h"

/**
 * dpu_ctl_mode_sel: Interface mode selection
 * DPU_CTL_MODE_SEL_VID:    Video mode interface
 * DPU_CTL_MODE_SEL_CMD:    Command mode interface
 */
enum dpu_ctl_mode_sel {
	DPU_CTL_MODE_SEL_VID = 0,
	DPU_CTL_MODE_SEL_CMD
};

struct dpu_hw_ctl;
/**
 * struct dpu_hw_stage_cfg - blending stage cfg
 * @stage : SSPP_ID at each stage
 * @multirect_index: index of the rectangle of SSPP.
 */
struct dpu_hw_stage_cfg {
	enum dpu_sspp stage[DPU_STAGE_MAX][PIPES_PER_STAGE];
	enum dpu_sspp_multirect_index multirect_index
					[DPU_STAGE_MAX][PIPES_PER_STAGE];
};

/**
 * struct dpu_hw_intf_cfg :Describes how the DPU writes data to output interface
 * @intf :                 Interface id
 * @intf_master:           Master interface id in the dual pipe topology
 * @mode_3d:               3d mux configuration
 * @merge_3d:              3d merge block used
 * @intf_mode_sel:         Interface mode, cmd / vid
 * @cdm:                   CDM block used
 * @stream_sel:            Stream selection for multi-stream interfaces
 * @dsc:                   DSC BIT masks used
 * @cwb:                   CWB BIT masks used
 */
struct dpu_hw_intf_cfg {
	enum dpu_intf intf;
	enum dpu_intf intf_master;
	enum dpu_wb wb;
	enum dpu_3d_blend_mode mode_3d;
	enum dpu_merge_3d merge_3d;
	enum dpu_ctl_mode_sel intf_mode_sel;
	enum dpu_cdm cdm;
	int stream_sel;
	unsigned int cwb;
	unsigned int dsc;
};

/**
 * struct dpu_hw_ctl_ops - Interface to the wb Hw driver functions
 * Assumption is these functions will be called after clocks are enabled
 */
struct dpu_hw_ctl_ops {
	/**
	 * kickoff hw operation for Sw controlled interfaces
	 * DSI cmd mode and WB interface are SW controlled
	 * @ctx       : ctl path ctx pointer
	 */
	void (*trigger_start)(struct dpu_hw_ctl *ctx);

	/**
	 * check if the ctl is started
	 * @ctx       : ctl path ctx pointer
	 * @Return: true if started, false if stopped
	 */
	bool (*is_started)(struct dpu_hw_ctl *ctx);

	/**
	 * kickoff prepare is in progress hw operation for sw
	 * controlled interfaces: DSI cmd mode and WB interface
	 * are SW controlled
	 * @ctx       : ctl path ctx pointer
	 */
	void (*trigger_pending)(struct dpu_hw_ctl *ctx);

	/**
	 * Clear the value of the cached pending_flush_mask
	 * No effect on hardware.
	 * Required to be implemented.
	 * @ctx       : ctl path ctx pointer
	 */
	void (*clear_pending_flush)(struct dpu_hw_ctl *ctx);

	/**
	 * Query the value of the cached pending_flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 */
	u32 (*get_pending_flush)(struct dpu_hw_ctl *ctx);

	/**
	 * OR in the given flushbits to the cached pending_flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @flushbits : module flushmask
	 */
	void (*update_pending_flush)(struct dpu_hw_ctl *ctx,
		u32 flushbits);

	/**
	 * OR in the given flushbits to the cached pending_(wb_)flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : writeback block index
	 */
	void (*update_pending_flush_wb)(struct dpu_hw_ctl *ctx,
		enum dpu_wb blk);

	/**
	 * OR in the given flushbits to the cached pending_(cwb_)flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : concurrent writeback block index
	 */
	void (*update_pending_flush_cwb)(struct dpu_hw_ctl *ctx,
		enum dpu_cwb blk);

	/**
	 * OR in the given flushbits to the cached pending_(intf_)flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : interface block index
	 */
	void (*update_pending_flush_intf)(struct dpu_hw_ctl *ctx,
		enum dpu_intf blk);

	/**
	 * OR in the given flushbits to the cached pending_(periph_)flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : interface block index
	 */
	void (*update_pending_flush_periph)(struct dpu_hw_ctl *ctx,
					    enum dpu_intf blk);

	/**
	 * OR in the given flushbits to the cached pending_(merge_3d_)flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : interface block index
	 */
	void (*update_pending_flush_merge_3d)(struct dpu_hw_ctl *ctx,
		enum dpu_merge_3d blk);

	/**
	 * OR in the given flushbits to the cached pending_flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : SSPP block index
	 */
	void (*update_pending_flush_sspp)(struct dpu_hw_ctl *ctx,
		enum dpu_sspp blk);

	/**
	 * OR in the given flushbits to the cached pending_flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : LM block index
	 */
	void (*update_pending_flush_mixer)(struct dpu_hw_ctl *ctx,
		enum dpu_lm blk);

	/**
	 * OR in the given flushbits to the cached pending_flush_mask
	 * No effect on hardware
	 * @ctx       : ctl path ctx pointer
	 * @blk       : DSPP block index
	 * @dspp_sub_blk : DSPP sub-block index
	 */
	void (*update_pending_flush_dspp)(struct dpu_hw_ctl *ctx,
		enum dpu_dspp blk, u32 dspp_sub_blk);

	/**
	 * OR in the given flushbits to the cached pending_(dsc_)flush_mask
	 * No effect on hardware
	 * @ctx: ctl path ctx pointer
	 * @blk: interface block index
	 */
	void (*update_pending_flush_dsc)(struct dpu_hw_ctl *ctx,
					 enum dpu_dsc blk);

	/**
	 * OR in the given flushbits to the cached pending_(cdm_)flush_mask
	 * No effect on hardware
	 * @ctx: ctl path ctx pointer
	 * @cdm_num: idx of cdm to be flushed
	 */
	void (*update_pending_flush_cdm)(struct dpu_hw_ctl *ctx, enum dpu_cdm cdm_num);

	/**
	 * Write the value of the pending_flush_mask to hardware
	 * @ctx       : ctl path ctx pointer
	 */
	void (*trigger_flush)(struct dpu_hw_ctl *ctx);

	/**
	 * Read the value of the flush register
	 * @ctx       : ctl path ctx pointer
	 * @Return: value of the ctl flush register.
	 */
	u32 (*get_flush_register)(struct dpu_hw_ctl *ctx);

	/**
	 * Setup ctl_path interface config
	 * @ctx
	 * @cfg    : interface config structure pointer
	 */
	void (*setup_intf_cfg)(struct dpu_hw_ctl *ctx,
		struct dpu_hw_intf_cfg *cfg);

	/**
	 * reset ctl_path interface config
	 * @ctx    : ctl path ctx pointer
	 * @cfg    : interface config structure pointer
	 */
	void (*reset_intf_cfg)(struct dpu_hw_ctl *ctx,
			struct dpu_hw_intf_cfg *cfg);

	int (*reset)(struct dpu_hw_ctl *c);

	/*
	 * wait_reset_status - checks ctl reset status
	 * @ctx       : ctl path ctx pointer
	 *
	 * This function checks the ctl reset status bit.
	 * If the reset bit is set, it keeps polling the status till the hw
	 * reset is complete.
	 * Returns: 0 on success or -error if reset incomplete within interval
	 */
	int (*wait_reset_status)(struct dpu_hw_ctl *ctx);

	/**
	 * Set all blend stages to disabled
	 * @ctx       : ctl path ctx pointer
	 */
	void (*clear_all_blendstages)(struct dpu_hw_ctl *ctx);

	/**
	 * Configure layer mixer to pipe configuration
	 * @ctx       : ctl path ctx pointer
	 * @lm        : layer mixer enumeration
	 * @cfg       : blend stage configuration
	 */
	void (*setup_blendstage)(struct dpu_hw_ctl *ctx,
		enum dpu_lm lm, struct dpu_hw_stage_cfg *cfg);

	void (*set_active_fetch_pipes)(struct dpu_hw_ctl *ctx,
		unsigned long *fetch_active);

	/**
	 * Set active pipes attached to this CTL
	 * @ctx: ctl path ctx pointer
	 * @active_pipes: bitmap of enum dpu_sspp
	 */
	void (*set_active_pipes)(struct dpu_hw_ctl *ctx,
				 unsigned long *active_pipes);

	/**
	 * Set active layer mixers attached to this CTL
	 * @ctx: ctl path ctx pointer
	 * @active_lms: bitmap of enum dpu_lm
	 */
	void (*set_active_lms)(struct dpu_hw_ctl *ctx,
			       unsigned long *active_lms);

};

/**
 * struct dpu_hw_ctl : CTL PATH driver object
 * @base: hardware block base structure
 * @hw: block register map object
 * @idx: control path index
 * @caps: control path capabilities
 * @mixer_count: number of mixers
 * @mixer_hw_caps: mixer hardware capabilities
 * @pending_flush_mask: storage for pending ctl_flush managed via ops
 * @pending_intf_flush_mask: pending INTF flush
 * @pending_wb_flush_mask: pending WB flush
 * @pending_cwb_flush_mask: pending CWB flush
 * @pending_dsc_flush_mask: pending DSC flush
 * @pending_cdm_flush_mask: pending CDM flush
 * @mdss_ver: MDSS revision information
 * @ops: operation list
 */
struct dpu_hw_ctl {
	struct dpu_hw_blk base;
	struct dpu_hw_blk_reg_map hw;

	/* ctl path */
	int idx;
	const struct dpu_ctl_cfg *caps;
	int mixer_count;
	const struct dpu_lm_cfg *mixer_hw_caps;
	u32 pending_flush_mask;
	u32 pending_intf_flush_mask;
	u32 pending_wb_flush_mask;
	u32 pending_cwb_flush_mask;
	u32 pending_periph_flush_mask;
	u32 pending_merge_3d_flush_mask;
	u32 pending_dspp_flush_mask[DSPP_MAX - DSPP_0];
	u32 pending_dsc_flush_mask;
	u32 pending_cdm_flush_mask;

	const struct dpu_mdss_version *mdss_ver;

	/* ops */
	struct dpu_hw_ctl_ops ops;
};

/**
 * dpu_hw_ctl - convert base object dpu_hw_base to container
 * @hw: Pointer to base hardware block
 * return: Pointer to hardware block container
 */
static inline struct dpu_hw_ctl *to_dpu_hw_ctl(struct dpu_hw_blk *hw)
{
	return container_of(hw, struct dpu_hw_ctl, base);
}

struct dpu_hw_ctl *dpu_hw_ctl_init(struct drm_device *dev,
				   const struct dpu_ctl_cfg *cfg,
				   void __iomem *addr,
				   const struct dpu_mdss_version *mdss_ver,
				   u32 mixer_count,
				   const struct dpu_lm_cfg *mixer);

#endif /*_DPU_HW_CTL_H */
