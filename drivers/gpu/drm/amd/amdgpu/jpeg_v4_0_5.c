/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "amdgpu.h"
#include "amdgpu_jpeg.h"
#include "amdgpu_pm.h"
#include "soc15.h"
#include "soc15d.h"
#include "jpeg_v2_0.h"
#include "jpeg_v4_0_5.h"
#include "mmsch_v4_0.h"

#include "vcn/vcn_4_0_5_offset.h"
#include "vcn/vcn_4_0_5_sh_mask.h"
#include "ivsrcid/vcn/irqsrcs_vcn_4_0.h"

#define mmUVD_DPG_LMA_CTL						regUVD_DPG_LMA_CTL
#define mmUVD_DPG_LMA_CTL_BASE_IDX					regUVD_DPG_LMA_CTL_BASE_IDX
#define mmUVD_DPG_LMA_DATA						regUVD_DPG_LMA_DATA
#define mmUVD_DPG_LMA_DATA_BASE_IDX					regUVD_DPG_LMA_DATA_BASE_IDX

#define regUVD_JPEG_PITCH_INTERNAL_OFFSET		0x401f
#define regJPEG_DEC_GFX10_ADDR_CONFIG_INTERNAL_OFFSET	0x4026
#define regJPEG_SYS_INT_EN_INTERNAL_OFFSET		0x4141
#define regJPEG_CGC_CTRL_INTERNAL_OFFSET		0x4161
#define regJPEG_CGC_GATE_INTERNAL_OFFSET		0x4160
#define regUVD_NO_OP_INTERNAL_OFFSET			0x0029

static const struct amdgpu_hwip_reg_entry jpeg_reg_list_4_0_5[] = {
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JPEG_POWER_STATUS),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JPEG_INT_STAT),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JRBC_RB_RPTR),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JRBC_RB_WPTR),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JRBC_RB_CNTL),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JRBC_RB_SIZE),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JRBC_STATUS),
	SOC15_REG_ENTRY_STR(JPEG, 0, regJPEG_DEC_ADDR_MODE),
	SOC15_REG_ENTRY_STR(JPEG, 0, regJPEG_DEC_GFX10_ADDR_CONFIG),
	SOC15_REG_ENTRY_STR(JPEG, 0, regJPEG_DEC_Y_GFX10_TILING_SURFACE),
	SOC15_REG_ENTRY_STR(JPEG, 0, regJPEG_DEC_UV_GFX10_TILING_SURFACE),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JPEG_PITCH),
	SOC15_REG_ENTRY_STR(JPEG, 0, regUVD_JPEG_UV_PITCH),
};

static void jpeg_v4_0_5_set_dec_ring_funcs(struct amdgpu_device *adev);
static void jpeg_v4_0_5_set_irq_funcs(struct amdgpu_device *adev);
static int jpeg_v4_0_5_set_powergating_state(struct amdgpu_ip_block *ip_block,
				enum amd_powergating_state state);
static void jpeg_v4_0_5_dec_ring_set_wptr(struct amdgpu_ring *ring);

static int amdgpu_ih_clientid_jpeg[] = {
	SOC15_IH_CLIENTID_VCN,
	SOC15_IH_CLIENTID_VCN1
};



/**
 * jpeg_v4_0_5_early_init - set function pointers
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Set ring and irq function pointers
 */
static int jpeg_v4_0_5_early_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;

	switch (amdgpu_ip_version(adev, UVD_HWIP, 0)) {
	case IP_VERSION(4, 0, 5):
		adev->jpeg.num_jpeg_inst = 1;
		break;
	case IP_VERSION(4, 0, 6):
		adev->jpeg.num_jpeg_inst = 2;
		break;
	default:
		DRM_DEV_ERROR(adev->dev,
			"Failed to init vcn ip block(UVD_HWIP:0x%x)\n",
			amdgpu_ip_version(adev, UVD_HWIP, 0));
		return -EINVAL;
	}

	adev->jpeg.num_jpeg_rings = 1;

	jpeg_v4_0_5_set_dec_ring_funcs(adev);
	jpeg_v4_0_5_set_irq_funcs(adev);

	return 0;
}

/**
 * jpeg_v4_0_5_sw_init - sw init for JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Load firmware and sw initialization
 */
static int jpeg_v4_0_5_sw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	struct amdgpu_ring *ring;
	int r, i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		/* JPEG TRAP */
		r = amdgpu_irq_add_id(adev, amdgpu_ih_clientid_jpeg[i],
				VCN_4_0__SRCID__JPEG_DECODE, &adev->jpeg.inst[i].irq);
		if (r)
			return r;

		/* JPEG DJPEG POISON EVENT */
		r = amdgpu_irq_add_id(adev, amdgpu_ih_clientid_jpeg[i],
			VCN_4_0__SRCID_DJPEG0_POISON, &adev->jpeg.inst[i].irq);
		if (r)
			return r;

		/* JPEG EJPEG POISON EVENT */
		r = amdgpu_irq_add_id(adev, amdgpu_ih_clientid_jpeg[i],
			VCN_4_0__SRCID_EJPEG0_POISON, &adev->jpeg.inst[i].irq);
		if (r)
			return r;
	}

	r = amdgpu_jpeg_sw_init(adev);
	if (r)
		return r;

	r = amdgpu_jpeg_resume(adev);
	if (r)
		return r;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ring = adev->jpeg.inst[i].ring_dec;
		ring->use_doorbell = true;
		ring->vm_hub = AMDGPU_MMHUB0(0);
		ring->doorbell_index = (adev->doorbell_index.vcn.vcn_ring0_1 << 1) + 1 + 8 * i;
		sprintf(ring->name, "jpeg_dec_%d", i);
		r = amdgpu_ring_init(adev, ring, 512, &adev->jpeg.inst[i].irq,
				     0, AMDGPU_RING_PRIO_DEFAULT, NULL);
		if (r)
			return r;

		adev->jpeg.internal.jpeg_pitch[0] = regUVD_JPEG_PITCH_INTERNAL_OFFSET;
		adev->jpeg.inst[i].external.jpeg_pitch[0] = SOC15_REG_OFFSET(JPEG, i, regUVD_JPEG_PITCH);
	}

	r = amdgpu_jpeg_reg_dump_init(adev, jpeg_reg_list_4_0_5, ARRAY_SIZE(jpeg_reg_list_4_0_5));
	if (r)
		return r;

	adev->jpeg.supported_reset =
		amdgpu_get_soft_full_reset_mask(&adev->jpeg.inst[0].ring_dec[0]);
	if (!amdgpu_sriov_vf(adev))
		adev->jpeg.supported_reset |= AMDGPU_RESET_TYPE_PER_QUEUE;
	r = amdgpu_jpeg_sysfs_reset_mask_init(adev);
	if (r)
		return r;

	return 0;
}

/**
 * jpeg_v4_0_5_sw_fini - sw fini for JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * JPEG suspend and free up sw allocation
 */
static int jpeg_v4_0_5_sw_fini(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int r;

	r = amdgpu_jpeg_suspend(adev);
	if (r)
		return r;

	amdgpu_jpeg_sysfs_reset_mask_fini(adev);
	r = amdgpu_jpeg_sw_fini(adev);

	return r;
}

/**
 * jpeg_v4_0_5_hw_init - start and test JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 */
static int jpeg_v4_0_5_hw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	struct amdgpu_ring *ring;
	int i, r = 0;

	// TODO: Enable ring test with DPG support
	if (adev->pg_flags & AMD_PG_SUPPORT_JPEG_DPG) {
		return 0;
	}

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ring = adev->jpeg.inst[i].ring_dec;
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	return 0;
}

/**
 * jpeg_v4_0_5_hw_fini - stop the hardware block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Stop the JPEG block, mark ring as not ready any more
 */
static int jpeg_v4_0_5_hw_fini(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int i;

	cancel_delayed_work_sync(&adev->jpeg.idle_work);

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		if (!amdgpu_sriov_vf(adev)) {
			if (adev->jpeg.cur_state != AMD_PG_STATE_GATE &&
			    RREG32_SOC15(JPEG, i, regUVD_JRBC_STATUS))
				jpeg_v4_0_5_set_powergating_state(ip_block, AMD_PG_STATE_GATE);
		}
	}
	return 0;
}

/**
 * jpeg_v4_0_5_suspend - suspend JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * HW fini and suspend JPEG block
 */
static int jpeg_v4_0_5_suspend(struct amdgpu_ip_block *ip_block)
{
	int r;

	r = jpeg_v4_0_5_hw_fini(ip_block);
	if (r)
		return r;

	r = amdgpu_jpeg_suspend(ip_block->adev);

	return r;
}

/**
 * jpeg_v4_0_5_resume - resume JPEG block
 *
 * @ip_block: Pointer to the amdgpu_ip_block for this hw instance.
 *
 * Resume firmware and hw init JPEG block
 */
static int jpeg_v4_0_5_resume(struct amdgpu_ip_block *ip_block)
{
	int r;

	r = amdgpu_jpeg_resume(ip_block->adev);
	if (r)
		return r;

	r = jpeg_v4_0_5_hw_init(ip_block);

	return r;
}

static void jpeg_v4_0_5_disable_clock_gating(struct amdgpu_device *adev, int inst)
{
	uint32_t data = 0;

	data = RREG32_SOC15(JPEG, inst, regJPEG_CGC_CTRL);
	if (adev->cg_flags & AMD_CG_SUPPORT_JPEG_MGCG) {
		data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
		data &= (~JPEG_CGC_CTRL__JPEG_DEC_MODE_MASK);
	} else {
		data &= ~JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
	}

	data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
	data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
	WREG32_SOC15(JPEG, inst, regJPEG_CGC_CTRL, data);

	data = RREG32_SOC15(JPEG, inst, regJPEG_CGC_GATE);
	data &= ~(JPEG_CGC_GATE__JPEG_DEC_MASK
		| JPEG_CGC_GATE__JPEG2_DEC_MASK
		| JPEG_CGC_GATE__JMCIF_MASK
		| JPEG_CGC_GATE__JRBBM_MASK);
	WREG32_SOC15(JPEG, inst, regJPEG_CGC_GATE, data);
}

static void jpeg_v4_0_5_enable_clock_gating(struct amdgpu_device *adev, int inst)
{
	uint32_t data = 0;

	data = RREG32_SOC15(JPEG, inst, regJPEG_CGC_CTRL);
	if (adev->cg_flags & AMD_CG_SUPPORT_JPEG_MGCG) {
		data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
		data |= JPEG_CGC_CTRL__JPEG_DEC_MODE_MASK;
	} else {
		data &= ~JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
	}

	data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
	data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
	WREG32_SOC15(JPEG, inst, regJPEG_CGC_CTRL, data);

	data = RREG32_SOC15(JPEG, inst, regJPEG_CGC_GATE);
	data |= (JPEG_CGC_GATE__JPEG_DEC_MASK
		|JPEG_CGC_GATE__JPEG2_DEC_MASK
		|JPEG_CGC_GATE__JMCIF_MASK
		|JPEG_CGC_GATE__JRBBM_MASK);
	WREG32_SOC15(JPEG, inst, regJPEG_CGC_GATE, data);
}

static void jpeg_engine_4_0_5_dpg_clock_gating_mode(struct amdgpu_device *adev,
			int inst_idx, uint8_t indirect)
{
	uint32_t data = 0;

	if (adev->cg_flags & AMD_CG_SUPPORT_JPEG_MGCG)
		data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
	else
		data |= 0 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;

	data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
	data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
	WREG32_SOC15_JPEG_DPG_MODE(inst_idx, regJPEG_CGC_CTRL_INTERNAL_OFFSET, data, indirect);

	data = 0;
	WREG32_SOC15_JPEG_DPG_MODE(inst_idx, regJPEG_CGC_GATE_INTERNAL_OFFSET,
				data, indirect);
}

static int jpeg_v4_0_5_disable_static_power_gating(struct amdgpu_device *adev, int inst)
{
	if (adev->pg_flags & AMD_PG_SUPPORT_JPEG) {
		WREG32(SOC15_REG_OFFSET(JPEG, inst, regUVD_IPX_DLDO_CONFIG),
			1 << UVD_IPX_DLDO_CONFIG__ONO1_PWR_CONFIG__SHIFT);
		SOC15_WAIT_ON_RREG(JPEG, inst, regUVD_IPX_DLDO_STATUS,
			0, UVD_IPX_DLDO_STATUS__ONO1_PWR_STATUS_MASK);
	}

	/* disable anti hang mechanism */
	WREG32_P(SOC15_REG_OFFSET(JPEG, inst, regUVD_JPEG_POWER_STATUS), 0,
		~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);

	/* keep the JPEG in static PG mode */
	WREG32_P(SOC15_REG_OFFSET(JPEG, inst, regUVD_JPEG_POWER_STATUS), 0,
		~UVD_JPEG_POWER_STATUS__JPEG_PG_MODE_MASK);

	return 0;
}

static int jpeg_v4_0_5_enable_static_power_gating(struct amdgpu_device *adev, int inst)
{
	/* enable anti hang mechanism */
	WREG32_P(SOC15_REG_OFFSET(JPEG, inst, regUVD_JPEG_POWER_STATUS),
		UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK,
		~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);

	if (adev->pg_flags & AMD_PG_SUPPORT_JPEG) {
		WREG32(SOC15_REG_OFFSET(JPEG, inst, regUVD_IPX_DLDO_CONFIG),
			2 << UVD_IPX_DLDO_CONFIG__ONO1_PWR_CONFIG__SHIFT);
		SOC15_WAIT_ON_RREG(JPEG, inst, regUVD_IPX_DLDO_STATUS,
			1 << UVD_IPX_DLDO_STATUS__ONO1_PWR_STATUS__SHIFT,
			UVD_IPX_DLDO_STATUS__ONO1_PWR_STATUS_MASK);
	}

	return 0;
}

/**
 * jpeg_v4_0_5_start_dpg_mode - Jpeg start with dpg mode
 *
 * @adev: amdgpu_device pointer
 * @inst_idx: instance number index
 * @indirect: indirectly write sram
 *
 * Start JPEG block with dpg mode
 */
static void jpeg_v4_0_5_start_dpg_mode(struct amdgpu_device *adev, int inst_idx, bool indirect)
{
	struct amdgpu_ring *ring = adev->jpeg.inst[inst_idx].ring_dec;
	uint32_t reg_data = 0;

	/* enable anti hang mechanism */
	reg_data = RREG32_SOC15(JPEG, inst_idx, regUVD_JPEG_POWER_STATUS);
	reg_data &= ~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK;
	reg_data |=  0x1;
	WREG32_SOC15(JPEG, inst_idx, regUVD_JPEG_POWER_STATUS, reg_data);

	if (adev->pg_flags & AMD_PG_SUPPORT_JPEG) {
		WREG32(SOC15_REG_OFFSET(JPEG, inst_idx, regUVD_IPX_DLDO_CONFIG),
			2 << UVD_IPX_DLDO_CONFIG__ONO1_PWR_CONFIG__SHIFT);
		SOC15_WAIT_ON_RREG(JPEG, inst_idx, regUVD_IPX_DLDO_STATUS,
			1 << UVD_IPX_DLDO_STATUS__ONO1_PWR_STATUS__SHIFT,
			UVD_IPX_DLDO_STATUS__ONO1_PWR_STATUS_MASK);
	}

	reg_data = RREG32_SOC15(JPEG, inst_idx, regUVD_JPEG_POWER_STATUS);
	reg_data |= UVD_JPEG_POWER_STATUS__JPEG_PG_MODE_MASK;
	WREG32_SOC15(JPEG, inst_idx, regUVD_JPEG_POWER_STATUS, reg_data);

	if (indirect)
		adev->jpeg.inst[inst_idx].dpg_sram_curr_addr =
					(uint32_t *)adev->jpeg.inst[inst_idx].dpg_sram_cpu_addr;

	jpeg_engine_4_0_5_dpg_clock_gating_mode(adev, inst_idx, indirect);

	/* MJPEG global tiling registers */
	WREG32_SOC15_JPEG_DPG_MODE(inst_idx, regJPEG_DEC_GFX10_ADDR_CONFIG_INTERNAL_OFFSET,
	adev->gfx.config.gb_addr_config, indirect);
	/* enable System Interrupt for JRBC */
	WREG32_SOC15_JPEG_DPG_MODE(inst_idx, regJPEG_SYS_INT_EN_INTERNAL_OFFSET,
	JPEG_SYS_INT_EN__DJRBC_MASK, indirect);

	/* add nop to workaround PSP size check */
	WREG32_SOC15_JPEG_DPG_MODE(inst_idx, regUVD_NO_OP_INTERNAL_OFFSET, 0, indirect);

	if (indirect)
		amdgpu_jpeg_psp_update_sram(adev, inst_idx, 0);

	WREG32_SOC15(JPEG, inst_idx, regUVD_LMI_JRBC_RB_VMID, 0);
	WREG32_SOC15(JPEG, inst_idx, regUVD_JRBC_RB_CNTL, (0x00000001L | 0x00000002L));
	WREG32_SOC15(JPEG, inst_idx, regUVD_LMI_JRBC_RB_64BIT_BAR_LOW,
		lower_32_bits(ring->gpu_addr));
	WREG32_SOC15(JPEG, inst_idx, regUVD_LMI_JRBC_RB_64BIT_BAR_HIGH,
		upper_32_bits(ring->gpu_addr));
	WREG32_SOC15(JPEG, inst_idx, regUVD_JRBC_RB_RPTR, 0);
	WREG32_SOC15(JPEG, inst_idx, regUVD_JRBC_RB_WPTR, 0);
	WREG32_SOC15(JPEG, inst_idx, regUVD_JRBC_RB_CNTL, 0x00000002L);
	WREG32_SOC15(JPEG, inst_idx, regUVD_JRBC_RB_SIZE, ring->ring_size / 4);
	ring->wptr = RREG32_SOC15(JPEG, inst_idx, regUVD_JRBC_RB_WPTR);
}

/**
 * jpeg_v4_0_5_stop_dpg_mode - Jpeg stop with dpg mode
 *
 * @adev: amdgpu_device pointer
 * @inst_idx: instance number index
 *
 * Stop JPEG block with dpg mode
 */
static void jpeg_v4_0_5_stop_dpg_mode(struct amdgpu_device *adev, int inst_idx)
{
	uint32_t reg_data = 0;

	reg_data = RREG32_SOC15(JPEG, inst_idx, regUVD_JPEG_POWER_STATUS);
	reg_data &= ~UVD_JPEG_POWER_STATUS__JPEG_PG_MODE_MASK;
	WREG32_SOC15(JPEG, inst_idx, regUVD_JPEG_POWER_STATUS, reg_data);

}

/**
 * jpeg_v4_0_5_start - start JPEG block
 *
 * @adev: amdgpu_device pointer
 *
 * Setup and start the JPEG block
 */
static int jpeg_v4_0_5_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	int r, i;

	if (adev->pm.dpm_enabled)
		amdgpu_dpm_enable_jpeg(adev, true);

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ring = adev->jpeg.inst[i].ring_dec;
		/* doorbell programming is done for every playback */
		adev->nbio.funcs->vcn_doorbell_range(adev, ring->use_doorbell,
				(adev->doorbell_index.vcn.vcn_ring0_1 << 1) + 8 * i, i);

		WREG32_SOC15(VCN, i, regVCN_JPEG_DB_CTRL,
			ring->doorbell_index << VCN_JPEG_DB_CTRL__OFFSET__SHIFT |
			VCN_JPEG_DB_CTRL__EN_MASK);

		if (adev->pg_flags & AMD_PG_SUPPORT_JPEG_DPG) {
			jpeg_v4_0_5_start_dpg_mode(adev, i, adev->jpeg.indirect_sram);
			continue;
		}

		/* disable power gating */
		r = jpeg_v4_0_5_disable_static_power_gating(adev, i);
		if (r)
			return r;

		/* JPEG disable CGC */
		jpeg_v4_0_5_disable_clock_gating(adev, i);

		/* MJPEG global tiling registers */
		WREG32_SOC15(JPEG, i, regJPEG_DEC_GFX10_ADDR_CONFIG,
			adev->gfx.config.gb_addr_config);

		/* enable JMI channel */
		WREG32_P(SOC15_REG_OFFSET(JPEG, i, regUVD_JMI_CNTL), 0,
			~UVD_JMI_CNTL__SOFT_RESET_MASK);

		/* enable System Interrupt for JRBC */
		WREG32_P(SOC15_REG_OFFSET(JPEG, i, regJPEG_SYS_INT_EN),
			JPEG_SYS_INT_EN__DJRBC_MASK,
			~JPEG_SYS_INT_EN__DJRBC_MASK);

		WREG32_SOC15(JPEG, i, regUVD_LMI_JRBC_RB_VMID, 0);
		WREG32_SOC15(JPEG, i, regUVD_JRBC_RB_CNTL, (0x00000001L | 0x00000002L));
		WREG32_SOC15(JPEG, i, regUVD_LMI_JRBC_RB_64BIT_BAR_LOW,
			lower_32_bits(ring->gpu_addr));
		WREG32_SOC15(JPEG, i, regUVD_LMI_JRBC_RB_64BIT_BAR_HIGH,
			upper_32_bits(ring->gpu_addr));
		WREG32_SOC15(JPEG, i, regUVD_JRBC_RB_RPTR, 0);
		WREG32_SOC15(JPEG, i, regUVD_JRBC_RB_WPTR, 0);
		WREG32_SOC15(JPEG, i, regUVD_JRBC_RB_CNTL, 0x00000002L);
		WREG32_SOC15(JPEG, i, regUVD_JRBC_RB_SIZE, ring->ring_size / 4);
		ring->wptr = RREG32_SOC15(JPEG, i, regUVD_JRBC_RB_WPTR);
	}

	return 0;
}

/**
 * jpeg_v4_0_5_stop - stop JPEG block
 *
 * @adev: amdgpu_device pointer
 *
 * stop the JPEG block
 */
static int jpeg_v4_0_5_stop(struct amdgpu_device *adev)
{
	int r, i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		if (adev->pg_flags & AMD_PG_SUPPORT_JPEG_DPG) {
			jpeg_v4_0_5_stop_dpg_mode(adev, i);
			continue;
		}

		/* reset JMI */
		WREG32_P(SOC15_REG_OFFSET(JPEG, i, regUVD_JMI_CNTL),
			UVD_JMI_CNTL__SOFT_RESET_MASK,
			~UVD_JMI_CNTL__SOFT_RESET_MASK);

		jpeg_v4_0_5_enable_clock_gating(adev, i);

		/* enable power gating */
		r = jpeg_v4_0_5_enable_static_power_gating(adev, i);
		if (r)
			return r;
	}
	if (adev->pm.dpm_enabled)
		amdgpu_dpm_enable_jpeg(adev, false);

	return 0;
}

/**
 * jpeg_v4_0_5_dec_ring_get_rptr - get read pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware read pointer
 */
static uint64_t jpeg_v4_0_5_dec_ring_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	return RREG32_SOC15(JPEG, ring->me, regUVD_JRBC_RB_RPTR);
}

/**
 * jpeg_v4_0_5_dec_ring_get_wptr - get write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware write pointer
 */
static uint64_t jpeg_v4_0_5_dec_ring_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell)
		return *ring->wptr_cpu_addr;
	else
		return RREG32_SOC15(JPEG, ring->me, regUVD_JRBC_RB_WPTR);
}

/**
 * jpeg_v4_0_5_dec_ring_set_wptr - set write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Commits the write pointer to the hardware
 */
static void jpeg_v4_0_5_dec_ring_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell) {
		*ring->wptr_cpu_addr = lower_32_bits(ring->wptr);
		WDOORBELL32(ring->doorbell_index, lower_32_bits(ring->wptr));
	} else {
		WREG32_SOC15(JPEG, ring->me, regUVD_JRBC_RB_WPTR, lower_32_bits(ring->wptr));
	}
}

static bool jpeg_v4_0_5_is_idle(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int i, ret = 1;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		ret &= (((RREG32_SOC15(JPEG, i, regUVD_JRBC_STATUS) &
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK) ==
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK));
	}
	return ret;
}

static int jpeg_v4_0_5_wait_for_idle(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		return SOC15_WAIT_ON_RREG(JPEG, i, regUVD_JRBC_STATUS,
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK,
			UVD_JRBC_STATUS__RB_JOB_DONE_MASK);
	}

	return 0;
}

static int jpeg_v4_0_5_set_clockgating_state(struct amdgpu_ip_block *ip_block,
					  enum amd_clockgating_state state)
{
	struct amdgpu_device *adev = ip_block->adev;
	bool enable = (state == AMD_CG_STATE_GATE) ? true : false;
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		if (enable) {
			if (!jpeg_v4_0_5_is_idle(ip_block))
				return -EBUSY;

			jpeg_v4_0_5_enable_clock_gating(adev, i);
		} else {
			jpeg_v4_0_5_disable_clock_gating(adev, i);
		}
	}

	return 0;
}

static int jpeg_v4_0_5_set_powergating_state(struct amdgpu_ip_block *ip_block,
					  enum amd_powergating_state state)
{
	struct amdgpu_device *adev = ip_block->adev;
	int ret;

	if (amdgpu_sriov_vf(adev)) {
		adev->jpeg.cur_state = AMD_PG_STATE_UNGATE;
		return 0;
	}

	if (state == adev->jpeg.cur_state)
		return 0;

	if (state == AMD_PG_STATE_GATE)
		ret = jpeg_v4_0_5_stop(adev);
	else
		ret = jpeg_v4_0_5_start(adev);

	if (!ret)
		adev->jpeg.cur_state = state;

	return ret;
}

static int jpeg_v4_0_5_process_interrupt(struct amdgpu_device *adev,
				      struct amdgpu_irq_src *source,
				      struct amdgpu_iv_entry *entry)
{
	uint32_t ip_instance;

	DRM_DEBUG("IH: JPEG TRAP\n");

	switch (entry->client_id) {
	case SOC15_IH_CLIENTID_VCN:
		ip_instance = 0;
		break;
	case SOC15_IH_CLIENTID_VCN1:
		ip_instance = 1;
		break;
	default:
		DRM_ERROR("Unhandled client id: %d\n", entry->client_id);
		return 0;
	}

	switch (entry->src_id) {
	case VCN_4_0__SRCID__JPEG_DECODE:
		amdgpu_fence_process(adev->jpeg.inst[ip_instance].ring_dec);
		break;
	case VCN_4_0__SRCID_DJPEG0_POISON:
	case VCN_4_0__SRCID_EJPEG0_POISON:
		amdgpu_jpeg_process_poison_irq(adev, source, entry);
		break;
	default:
		DRM_DEV_ERROR(adev->dev, "Unhandled interrupt: %d %d\n",
			  entry->src_id, entry->src_data[0]);
		break;
	}

	return 0;
}

static int jpeg_v4_0_5_ring_reset(struct amdgpu_ring *ring,
				  unsigned int vmid,
				  struct amdgpu_fence *timedout_fence)
{
	int r;

	amdgpu_ring_reset_helper_begin(ring, timedout_fence);
	r = jpeg_v4_0_5_stop(ring->adev);
	if (r)
		return r;
	r = jpeg_v4_0_5_start(ring->adev);
	if (r)
		return r;
	return amdgpu_ring_reset_helper_end(ring, timedout_fence);
}

static const struct amd_ip_funcs jpeg_v4_0_5_ip_funcs = {
	.name = "jpeg_v4_0_5",
	.early_init = jpeg_v4_0_5_early_init,
	.sw_init = jpeg_v4_0_5_sw_init,
	.sw_fini = jpeg_v4_0_5_sw_fini,
	.hw_init = jpeg_v4_0_5_hw_init,
	.hw_fini = jpeg_v4_0_5_hw_fini,
	.suspend = jpeg_v4_0_5_suspend,
	.resume = jpeg_v4_0_5_resume,
	.is_idle = jpeg_v4_0_5_is_idle,
	.wait_for_idle = jpeg_v4_0_5_wait_for_idle,
	.set_clockgating_state = jpeg_v4_0_5_set_clockgating_state,
	.set_powergating_state = jpeg_v4_0_5_set_powergating_state,
	.dump_ip_state = amdgpu_jpeg_dump_ip_state,
	.print_ip_state = amdgpu_jpeg_print_ip_state,
};

static const struct amdgpu_ring_funcs jpeg_v4_0_5_dec_ring_vm_funcs = {
	.type = AMDGPU_RING_TYPE_VCN_JPEG,
	.align_mask = 0xf,
	.get_rptr = jpeg_v4_0_5_dec_ring_get_rptr,
	.get_wptr = jpeg_v4_0_5_dec_ring_get_wptr,
	.set_wptr = jpeg_v4_0_5_dec_ring_set_wptr,
	.parse_cs = jpeg_v2_dec_ring_parse_cs,
	.emit_frame_size =
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 6 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 8 +
		8 + /* jpeg_v4_0_5_dec_ring_emit_vm_flush */
		18 + 18 + /* jpeg_v4_0_5_dec_ring_emit_fence x2 vm fence */
		8 + 16,
	.emit_ib_size = 22, /* jpeg_v4_0_5_dec_ring_emit_ib */
	.emit_ib = jpeg_v2_0_dec_ring_emit_ib,
	.emit_fence = jpeg_v2_0_dec_ring_emit_fence,
	.emit_vm_flush = jpeg_v2_0_dec_ring_emit_vm_flush,
	.test_ring = amdgpu_jpeg_dec_ring_test_ring,
	.test_ib = amdgpu_jpeg_dec_ring_test_ib,
	.insert_nop = jpeg_v2_0_dec_ring_nop,
	.insert_start = jpeg_v2_0_dec_ring_insert_start,
	.insert_end = jpeg_v2_0_dec_ring_insert_end,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.begin_use = amdgpu_jpeg_ring_begin_use,
	.end_use = amdgpu_jpeg_ring_end_use,
	.emit_wreg = jpeg_v2_0_dec_ring_emit_wreg,
	.emit_reg_wait = jpeg_v2_0_dec_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = amdgpu_ring_emit_reg_write_reg_wait_helper,
	.reset = jpeg_v4_0_5_ring_reset,
};

static void jpeg_v4_0_5_set_dec_ring_funcs(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		adev->jpeg.inst[i].ring_dec->funcs = &jpeg_v4_0_5_dec_ring_vm_funcs;
		adev->jpeg.inst[i].ring_dec->me = i;
	}
}

static const struct amdgpu_irq_src_funcs jpeg_v4_0_5_irq_funcs = {
	.process = jpeg_v4_0_5_process_interrupt,
};

static void jpeg_v4_0_5_set_irq_funcs(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_inst; ++i) {
		if (adev->jpeg.harvest_config & (1 << i))
			continue;

		adev->jpeg.inst[i].irq.num_types = 1;
		adev->jpeg.inst[i].irq.funcs = &jpeg_v4_0_5_irq_funcs;
	}
}

const struct amdgpu_ip_block_version jpeg_v4_0_5_ip_block = {
	.type = AMD_IP_BLOCK_TYPE_JPEG,
	.major = 4,
	.minor = 0,
	.rev = 5,
	.funcs = &jpeg_v4_0_5_ip_funcs,
};

