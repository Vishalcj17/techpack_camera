// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>

#include <soc/qcom/scm.h>
#include <media/cam_custom.h>
#include <media/cam_sync.h>

#include "cam_sync_api.h"
#include "cam_smmu_api.h"
#include "cam_req_mgr_workq.h"
#include "cam_custom_hw_mgr.h"
#include "cam_packet_util.h"
#include "cam_debug_util.h"
#include "cam_cpas_api.h"
#include "cam_mem_mgr_api.h"
#include "cam_common_util.h"
#include "cam_hw.h"

static struct cam_custom_hw_mgr g_custom_hw_mgr;

static int cam_custom_mgr_get_hw_caps(void *hw_mgr_priv,
	void *hw_caps_args)
{
	int rc = 0;
	struct cam_custom_hw_mgr          *hw_mgr = hw_mgr_priv;
	struct cam_query_cap_cmd          *query = hw_caps_args;
	struct cam_custom_query_cap_cmd    custom_hw_cap;
	struct cam_hw_info                *cam_custom_hw;
	struct cam_hw_soc_info            *soc_info_hw;

	cam_custom_hw = (struct cam_hw_info *)
		g_custom_hw_mgr.custom_hw[0]->hw_priv;
	if (cam_custom_hw)
		soc_info_hw = &cam_custom_hw->soc_info;

	CAM_DBG(CAM_CUSTOM, "enter");

	if (query->handle_type != CAM_HANDLE_USER_POINTER)
		CAM_ERR(CAM_CUSTOM, "Wrong Args");

	if (copy_from_user(&custom_hw_cap,
		u64_to_user_ptr(query->caps_handle),
		sizeof(struct cam_custom_query_cap_cmd))) {
		rc = -EFAULT;
		return rc;
	}

	custom_hw_cap.device_iommu.non_secure = hw_mgr->img_iommu_hdl;
	custom_hw_cap.device_iommu.secure = -1;

	/* Initializing cdm handles to -1 */
	custom_hw_cap.cdm_iommu.non_secure = -1;
	custom_hw_cap.cdm_iommu.secure = -1;

	custom_hw_cap.num_dev = 1;
	custom_hw_cap.dev_caps[0].hw_type = 0;
	custom_hw_cap.dev_caps[0].hw_version = 0;

	if (copy_to_user(u64_to_user_ptr(query->caps_handle),
		&custom_hw_cap, sizeof(struct cam_custom_query_cap_cmd)))
		rc = -EFAULT;

	CAM_DBG(CAM_CUSTOM, "exit rc :%d", rc);
	return rc;
}

enum cam_custom_hw_resource_state
	cam_custom_hw_mgr_get_custom_res_state(
	uint32_t						in_rsrc_state)
{
	enum cam_custom_hw_resource_state     rsrc_state;

	CAM_DBG(CAM_CUSTOM, "rsrc_state %x", in_rsrc_state);

	switch (in_rsrc_state) {
	case CAM_ISP_RESOURCE_STATE_UNAVAILABLE:
		rsrc_state = CAM_CUSTOM_HW_RESOURCE_STATE_UNAVAILABLE;
		break;
	case CAM_ISP_RESOURCE_STATE_AVAILABLE:
		rsrc_state = CAM_CUSTOM_HW_RESOURCE_STATE_AVAILABLE;
		break;
	case CAM_ISP_RESOURCE_STATE_RESERVED:
		rsrc_state = CAM_CUSTOM_HW_RESOURCE_STATE_RESERVED;
		break;
	case CAM_ISP_RESOURCE_STATE_INIT_HW:
		rsrc_state = CAM_CUSTOM_HW_RESOURCE_STATE_INIT_HW;
		break;
	case CAM_ISP_RESOURCE_STATE_STREAMING:
		rsrc_state = CAM_CUSTOM_HW_RESOURCE_STATE_STREAMING;
		break;
	default:
		rsrc_state = CAM_CUSTOM_HW_RESOURCE_STATE_UNAVAILABLE;
		CAM_DBG(CAM_CUSTOM, "invalid rsrc type");
		break;
	}

	return rsrc_state;
}

enum cam_isp_resource_state
	cam_custom_hw_mgr_get_isp_res_state(
	uint32_t						in_rsrc_state)
{
	enum cam_isp_resource_state     rsrc_state;

	CAM_DBG(CAM_CUSTOM, "rsrc_state %x", in_rsrc_state);

	switch (in_rsrc_state) {
	case CAM_CUSTOM_HW_RESOURCE_STATE_UNAVAILABLE:
		rsrc_state = CAM_ISP_RESOURCE_STATE_UNAVAILABLE;
		break;
	case CAM_CUSTOM_HW_RESOURCE_STATE_AVAILABLE:
		rsrc_state = CAM_ISP_RESOURCE_STATE_AVAILABLE;
		break;
	case CAM_CUSTOM_HW_RESOURCE_STATE_RESERVED:
		rsrc_state = CAM_ISP_RESOURCE_STATE_RESERVED;
		break;
	case CAM_CUSTOM_HW_RESOURCE_STATE_INIT_HW:
		rsrc_state = CAM_ISP_RESOURCE_STATE_INIT_HW;
		break;
	case CAM_CUSTOM_HW_RESOURCE_STATE_STREAMING:
		rsrc_state = CAM_ISP_RESOURCE_STATE_STREAMING;
		break;
	default:
		rsrc_state = CAM_ISP_RESOURCE_STATE_UNAVAILABLE;
		CAM_DBG(CAM_CUSTOM, "invalid rsrc type");
		break;
	}

	return rsrc_state;
}

enum cam_isp_resource_type
	cam_custom_hw_mgr_get_isp_res_type(
	enum cam_custom_hw_mgr_res_type res_type)
{
	switch (res_type) {
	case CAM_CUSTOM_CID_HW:
		return CAM_ISP_RESOURCE_CID;
	case CAM_CUSTOM_CSID_HW:
		return CAM_ISP_RESOURCE_PIX_PATH;
	default:
		return CAM_ISP_RESOURCE_MAX;
	}
}

static int cam_custom_hw_mgr_deinit_hw_res(
	struct cam_custom_hw_mgr_res *hw_mgr_res)
{
	int rc = -1;
	struct cam_isp_resource_node *isp_rsrc_node = NULL;
	struct cam_hw_intf			 *hw_intf = NULL;

	isp_rsrc_node =
		(struct cam_isp_resource_node *)hw_mgr_res->rsrc_node;
	if (!isp_rsrc_node) {
		CAM_ERR(CAM_CUSTOM, "Invalid args");
		return -EINVAL;
	}

	hw_intf = isp_rsrc_node->hw_intf;
	if (hw_intf->hw_ops.deinit) {
		CAM_DBG(CAM_CUSTOM, "DEINIT HW for res_id:%u",
			hw_mgr_res->res_id);
		rc = hw_intf->hw_ops.deinit(hw_intf->hw_priv,
			isp_rsrc_node, sizeof(struct cam_isp_resource_node));
		if (rc)
			goto err;
	}

	return 0;

err:
	CAM_DBG(CAM_CUSTOM, "DEINIT HW failed for res_id:%u",
		hw_mgr_res->res_id);
	return rc;
}

static int cam_custom_hw_mgr_stop_hw_res(
	struct cam_custom_hw_mgr_res *hw_mgr_res)
{
	int rc = -1;
	struct cam_csid_hw_stop_args  stop_cmd;
	struct cam_isp_resource_node *isp_rsrc_node = NULL;
	struct cam_hw_intf			 *hw_intf = NULL;

	isp_rsrc_node =
		(struct cam_isp_resource_node *)hw_mgr_res->rsrc_node;
	if (!isp_rsrc_node) {
		CAM_ERR(CAM_CUSTOM, "Invalid args");
		return -EINVAL;
	}

	hw_intf = isp_rsrc_node->hw_intf;
	if (hw_intf->hw_ops.stop) {
		CAM_DBG(CAM_CUSTOM, "STOP HW for res_id:%u",
			hw_mgr_res->res_id);
		stop_cmd.num_res = 1;
		stop_cmd.node_res = &isp_rsrc_node;
		stop_cmd.stop_cmd = CAM_CSID_HALT_AT_FRAME_BOUNDARY;
		rc = hw_intf->hw_ops.stop(hw_intf->hw_priv,
			&stop_cmd, sizeof(struct cam_csid_hw_stop_args));
		if (rc)
			goto err;
	}

	return 0;

err:
	CAM_DBG(CAM_CUSTOM, "STOP HW failed for res_id:%u",
		hw_mgr_res->res_id);
	return rc;
}

static int cam_custom_mgr_stop_hw(void *hw_mgr_priv, void *stop_hw_args)
{
	int                               rc        = 0;
	struct cam_hw_stop_args          *stop_args = stop_hw_args;
	struct cam_custom_hw_mgr_res     *hw_mgr_res;
	struct cam_custom_hw_mgr_ctx     *ctx;

	if (!hw_mgr_priv || !stop_hw_args) {
		CAM_ERR(CAM_CUSTOM, "Invalid arguments");
		return -EINVAL;
	}

	ctx = (struct cam_custom_hw_mgr_ctx *)
		stop_args->ctxt_to_hw_map;

	if (!ctx || !ctx->ctx_in_use) {
		CAM_ERR(CAM_CUSTOM, "Invalid context is used");
		return -EPERM;
	}

	CAM_DBG(CAM_CUSTOM, " Enter...ctx id:%d", ctx->ctx_index);

	/* Stop custom cid here */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_cid, list) {
		rc = cam_custom_hw_mgr_stop_hw_res(hw_mgr_res);
		if (rc)
			CAM_ERR(CAM_CUSTOM, "failed to stop hw %d",
				hw_mgr_res->res_id);
	}

	/* Stop custom csid here */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_csid, list) {
		rc = cam_custom_hw_mgr_stop_hw_res(hw_mgr_res);
		if (rc) {
			CAM_ERR(CAM_CUSTOM, "failed to stop hw %d",
				hw_mgr_res->res_id);
		}
	}


	/* stop custom hw here */

	/* Deinit custom cid here */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_cid, list) {
		rc = cam_custom_hw_mgr_deinit_hw_res(hw_mgr_res);
		if (rc)
			CAM_ERR(CAM_CUSTOM, "failed to stop hw %d",
			hw_mgr_res->res_id);
	}

	/* Deinit custom csid here */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_csid, list) {
		rc = cam_custom_hw_mgr_deinit_hw_res(hw_mgr_res);
		if (rc) {
			CAM_ERR(CAM_CUSTOM, "failed to stop hw %d",
				hw_mgr_res->res_id);
		}
	}

	/* deinit custom rsrc */

	return rc;
}

static int cam_custom_hw_mgr_init_hw_res(
	struct cam_custom_hw_mgr_res *hw_mgr_res)
{
	int rc = -1;
	struct cam_isp_resource_node *isp_rsrc_node = NULL;
	struct cam_hw_intf			 *hw_intf = NULL;

	isp_rsrc_node =
		(struct cam_isp_resource_node *)hw_mgr_res->rsrc_node;
	if (!isp_rsrc_node) {
		CAM_ERR(CAM_CUSTOM, "Invalid args");
		return -EINVAL;
	}

	hw_intf = isp_rsrc_node->hw_intf;
	if (hw_intf->hw_ops.init) {
		CAM_DBG(CAM_CUSTOM, "INIT HW for res_id:%u",
			hw_mgr_res->res_id);
		rc = hw_intf->hw_ops.init(hw_intf->hw_priv,
			isp_rsrc_node, sizeof(struct cam_isp_resource_node));
		if (rc)
			goto err;
	}

	return 0;

err:
	CAM_DBG(CAM_CUSTOM, "INIT HW failed for res_id:%u",
		hw_mgr_res->res_id);
	return rc;
}

static int cam_custom_hw_mgr_start_hw_res(
	struct cam_custom_hw_mgr_res *hw_mgr_res)
{
	int rc = -1;
	struct cam_isp_resource_node *isp_rsrc_node = NULL;
	struct cam_hw_intf			 *hw_intf = NULL;

	isp_rsrc_node =
		(struct cam_isp_resource_node *)hw_mgr_res->rsrc_node;
	if (!isp_rsrc_node) {
		CAM_ERR(CAM_CUSTOM, "Invalid args");
		return -EINVAL;
	}

	hw_intf = isp_rsrc_node->hw_intf;
	if (hw_intf->hw_ops.start) {
		CAM_DBG(CAM_CUSTOM, "Start HW for res_id:%u",
			hw_mgr_res->res_id);
		rc = hw_intf->hw_ops.start(hw_intf->hw_priv,
			isp_rsrc_node, sizeof(struct cam_isp_resource_node));
		if (rc)
			goto err;
	}

	return 0;

err:
	CAM_DBG(CAM_CUSTOM, "START HW failed for res_id:%u",
		hw_mgr_res->res_id);
	return rc;
}

static int cam_custom_mgr_start_hw(void *hw_mgr_priv,
	void *start_hw_args)
{
	int                                      rc = 0;
	struct cam_hw_config_args               *hw_config;
	struct cam_hw_stop_args                  stop_args;
	struct cam_custom_hw_mgr_res            *hw_mgr_res;
	struct cam_custom_hw_mgr_ctx            *ctx;

	if (!hw_mgr_priv || !start_hw_args) {
		CAM_ERR(CAM_CUSTOM, "Invalid arguments");
		return -EINVAL;
	}

	hw_config = (struct cam_hw_config_args *)start_hw_args;

	ctx = (struct cam_custom_hw_mgr_ctx *)
		hw_config->ctxt_to_hw_map;
	if (!ctx || !ctx->ctx_in_use) {
		CAM_ERR(CAM_CUSTOM, "Invalid context is used");
		return -EPERM;
	}

	CAM_DBG(CAM_CUSTOM, "Enter... ctx id:%d",
		ctx->ctx_index);

	/* Init custom cid */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_cid, list) {
		rc = cam_custom_hw_mgr_init_hw_res(hw_mgr_res);
		if (rc) {
			CAM_ERR(CAM_ISP, "Can not INIT CID(id :%d)",
				hw_mgr_res->res_id);
			goto deinit_hw;
		}
	}

	/* Init custom csid */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_csid, list) {
		rc = cam_custom_hw_mgr_init_hw_res(hw_mgr_res);
		if (rc) {
			CAM_ERR(CAM_ISP, "Can not INIT CSID(id :%d)",
				hw_mgr_res->res_id);
			goto deinit_hw;
		}
	}


	/* Init custom hw here */

	/* Apply init config */

	/* Start custom HW first */
	if (rc < 0)
		goto err;

	/* Start custom csid */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_csid, list) {
		rc = cam_custom_hw_mgr_start_hw_res(hw_mgr_res);
		if (rc) {
			CAM_ERR(CAM_ISP, "Can not START CSID(id :%d)",
				hw_mgr_res->res_id);
			goto err;
		}
	}

	/* Start custom cid */
	list_for_each_entry(hw_mgr_res,
		&ctx->res_list_custom_cid, list) {
		rc = cam_custom_hw_mgr_start_hw_res(hw_mgr_res);
		if (rc) {
			CAM_ERR(CAM_ISP, "Can not START CID(id :%d)",
				hw_mgr_res->res_id);
			goto err;
		}
	}

	CAM_DBG(CAM_CUSTOM, "Start success for ctx id:%d", ctx->ctx_index);
	return 0;

err:
	stop_args.ctxt_to_hw_map = hw_config->ctxt_to_hw_map;
	cam_custom_mgr_stop_hw(hw_mgr_priv, &stop_args);
deinit_hw:
	/* deinit the hw previously initialized */
	CAM_DBG(CAM_CUSTOM, "Exit...(rc=%d)", rc);
	return rc;
}

static int cam_custom_mgr_read(void *hw_mgr_priv, void *read_args)
{
	return -EPERM;
}

static int cam_custom_mgr_write(void *hw_mgr_priv, void *write_args)
{
	return -EPERM;
}

static int cam_custom_hw_mgr_put_ctx(
	struct list_head                 *src_list,
	struct cam_custom_hw_mgr_ctx    **custom_ctx)
{
	struct cam_custom_hw_mgr_ctx *ctx_ptr  = NULL;

	ctx_ptr = *custom_ctx;
	if (ctx_ptr)
		list_add_tail(&ctx_ptr->list, src_list);
	*custom_ctx = NULL;
	return 0;
}

static int cam_custom_hw_mgr_get_ctx(
	struct list_head                *src_list,
	struct cam_custom_hw_mgr_ctx       **custom_ctx)
{
	struct cam_custom_hw_mgr_ctx *ctx_ptr  = NULL;

	if (!list_empty(src_list)) {
		ctx_ptr = list_first_entry(src_list,
			struct cam_custom_hw_mgr_ctx, list);
		list_del_init(&ctx_ptr->list);
	} else {
		CAM_ERR(CAM_CUSTOM, "No more free custom hw mgr ctx");
		return -EINVAL;
	}
	*custom_ctx = ctx_ptr;
	memset(ctx_ptr->sub_hw_list, 0,
		sizeof(struct cam_custom_hw_mgr_res) *
		CAM_CUSTOM_HW_RES_MAX);

	return 0;
}

static int cam_custom_hw_mgr_put_res(
	struct list_head                *src_list,
	struct cam_custom_hw_mgr_res   **res)
{
	struct cam_custom_hw_mgr_res *res_ptr = NULL;

	res_ptr = *res;
	if (res_ptr)
		list_add_tail(&res_ptr->list, src_list);

	return 0;
}

static int cam_custom_hw_mgr_get_res(
	struct list_head                *src_list,
	struct cam_custom_hw_mgr_res   **res)
{
	int rc = 0;
	struct cam_custom_hw_mgr_res *res_ptr = NULL;

	if (!list_empty(src_list)) {
		res_ptr = list_first_entry(src_list,
			struct cam_custom_hw_mgr_res, list);
		list_del_init(&res_ptr->list);
	} else {
		CAM_ERR(CAM_CUSTOM, "No more free custom ctx rsrc");
		rc = -1;
	}
	*res = res_ptr;

	return rc;
}

static enum cam_ife_pix_path_res_id
	cam_custom_hw_mgr_get_csid_res_type(
	uint32_t						out_port_type)
{
	enum cam_ife_pix_path_res_id path_id;

	CAM_DBG(CAM_CUSTOM, "out_port_type %x", out_port_type);

	switch (out_port_type) {
	case CAM_CUSTOM_OUT_RES_UDI_0:
		path_id = CAM_IFE_PIX_PATH_RES_UDI_0;
		break;
	case CAM_CUSTOM_OUT_RES_UDI_1:
		path_id = CAM_IFE_PIX_PATH_RES_UDI_1;
		break;
	case CAM_CUSTOM_OUT_RES_UDI_2:
		path_id = CAM_IFE_PIX_PATH_RES_UDI_2;
		break;
	default:
		path_id = CAM_IFE_PIX_PATH_RES_MAX;
		CAM_DBG(CAM_CUSTOM, "maximum rdi output type exceeded");
		break;
	}

	CAM_DBG(CAM_CUSTOM, "out_port %x path_id %d", out_port_type, path_id);

	return path_id;
}

static int cam_custom_hw_mgr_acquire_cid_res(
	struct cam_custom_hw_mgr_ctx           *custom_ctx,
	struct cam_isp_in_port_generic_info    *in_port,
	struct cam_custom_hw_mgr_res          **cid_res,
	enum cam_ife_pix_path_res_id            path_res_id,
	struct cam_isp_resource_node          **cid_rsrc_node)
{
	int rc = -1;
	int i;
	struct cam_custom_hw_mgr             *custom_hw_mgr;
	struct cam_hw_intf                   *hw_intf;
	struct cam_custom_hw_mgr_res         *cid_res_temp;
	struct cam_csid_hw_reserve_resource_args  csid_acquire;
	struct cam_isp_resource_node          *isp_rsrc_node;
	struct cam_isp_out_port_generic_info *out_port = NULL;

	custom_hw_mgr = custom_ctx->hw_mgr;
	*cid_res = NULL;

	rc = cam_custom_hw_mgr_get_res(
		&custom_ctx->free_res_list, cid_res);
	if (rc) {
		CAM_ERR(CAM_CUSTOM, "No more free hw mgr resource");
		goto end;
	}

	memset(&csid_acquire, 0, sizeof(csid_acquire));
	cid_res_temp = *cid_res;
	csid_acquire.res_type = CAM_ISP_RESOURCE_CID;
	csid_acquire.in_port = in_port;
	csid_acquire.res_id =  path_res_id;
	csid_acquire.node_res = NULL;
	CAM_DBG(CAM_CUSTOM, "path_res_id %d", path_res_id);

	if (in_port->num_out_res)
		out_port = &(in_port->data[0]);

	for (i = 0; i < CAM_CUSTOM_CSID_HW_MAX; i++) {
		if (!custom_hw_mgr->csid_devices[i])
			continue;

		hw_intf = custom_hw_mgr->csid_devices[i];
		rc = hw_intf->hw_ops.reserve(hw_intf->hw_priv,
				&csid_acquire, sizeof(csid_acquire));
		/* since there is a need of 1 cid at this stage */
			if (rc)
				continue;
			else
				break;

	}

	if (!csid_acquire.node_res) {
		CAM_ERR(CAM_CUSTOM,
			"Can not acquire custom cid resource for path %d",
			path_res_id);
		rc = -EAGAIN;
		goto put_res;
	}

	*cid_rsrc_node = csid_acquire.node_res;
	isp_rsrc_node = csid_acquire.node_res;
	cid_res_temp->rsrc_node = isp_rsrc_node;
	cid_res_temp->res_type = CAM_CUSTOM_CID_HW;
	cid_res_temp->res_id = isp_rsrc_node->res_id;
	cam_custom_hw_mgr_put_res(&custom_ctx->res_list_custom_cid,
		&cid_res_temp);

	CAM_DBG(CAM_CUSTOM, "CID acquired successfully %u",
		isp_rsrc_node->res_id);

	return 0;

put_res:
	cam_custom_hw_mgr_put_res(&custom_ctx->free_res_list, cid_res);
end:
	return rc;

}

static int cam_custom_hw_mgr_acquire_csid_res(
	struct cam_custom_hw_mgr_ctx        *custom_ctx,
	struct cam_isp_in_port_generic_info *in_port_info)
{
	int rc = 0, i = 0;
	struct cam_custom_hw_mgr                *custom_hw_mgr;
	struct cam_isp_out_port_generic_info    *out_port;
	struct cam_custom_hw_mgr_res            *custom_csid_res;
	struct cam_custom_hw_mgr_res            *custom_cid_res;
	struct cam_hw_intf                      *hw_intf;
	struct cam_csid_hw_reserve_resource_args custom_csid_acquire;
	enum cam_ife_pix_path_res_id             path_res_id;
	struct cam_isp_resource_node            *isp_rsrc_node;
	struct cam_isp_resource_node            *cid_rsrc_node = NULL;

	custom_hw_mgr = custom_ctx->hw_mgr;

	for (i = 0; i < in_port_info->num_out_res; i++) {
		out_port = &in_port_info->data[i];
		path_res_id = cam_custom_hw_mgr_get_csid_res_type(
			out_port->res_type);

		if (path_res_id == CAM_IFE_PIX_PATH_RES_MAX) {
			CAM_WARN(CAM_CUSTOM, "Invalid out port res_type %u",
				out_port->res_type);
			continue;
		}

		rc = cam_custom_hw_mgr_acquire_cid_res(custom_ctx, in_port_info,
			&custom_cid_res, path_res_id, &cid_rsrc_node);
		if (rc) {
			CAM_ERR(CAM_CUSTOM, "No free cid rsrc %d", rc);
			goto end;
		}

		rc = cam_custom_hw_mgr_get_res(&custom_ctx->free_res_list,
			&custom_csid_res);
		if (rc) {
			CAM_ERR(CAM_CUSTOM, "No more free hw mgr rsrc");
			goto end;
		}

		memset(&custom_csid_acquire, 0, sizeof(custom_csid_acquire));
		custom_csid_acquire.res_id = path_res_id;
		custom_csid_acquire.res_type = CAM_ISP_RESOURCE_PIX_PATH;
		custom_csid_acquire.cid = cid_rsrc_node->res_id;
		custom_csid_acquire.in_port = in_port_info;
		custom_csid_acquire.out_port = out_port;
		custom_csid_acquire.sync_mode = 0;
		custom_csid_acquire.node_res = NULL;

		hw_intf = cid_rsrc_node->hw_intf;
		rc = hw_intf->hw_ops.reserve(hw_intf->hw_priv,
			&custom_csid_acquire, sizeof(custom_csid_acquire));
		if (rc) {
			CAM_ERR(CAM_CUSTOM,
				"Custom csid acquire failed for hw_idx %u rc %d",
				hw_intf->hw_idx, rc);
			goto put_res;
		}

		if (custom_csid_acquire.node_res == NULL) {
			CAM_ERR(CAM_CUSTOM, "Acquire custom csid failed");
			rc = -EAGAIN;
			goto put_res;
		}

		isp_rsrc_node = custom_csid_acquire.node_res;
		custom_csid_res->rsrc_node = isp_rsrc_node;
		custom_csid_res->res_type = CAM_CUSTOM_CSID_HW;
		custom_csid_res->res_id = custom_csid_acquire.res_id;
		cam_custom_hw_mgr_put_res(
			&custom_ctx->res_list_custom_csid,
			&custom_csid_res);
		CAM_DBG(CAM_CUSTOM, "Custom CSID acquired for path %d",
			path_res_id);
	}

	return 0;

put_res:
	cam_custom_hw_mgr_put_res(&custom_ctx->free_res_list,
		&custom_csid_res);
end:
	return rc;
}

static int cam_custom_hw_mgr_free_hw_res(
	struct cam_custom_hw_mgr_res   *hw_mgr_res)
{
	int rc = 0;
	struct cam_isp_resource_node *isp_rsrc_node = NULL;
	struct cam_hw_intf			 *hw_intf = NULL;

	isp_rsrc_node =
		(struct cam_isp_resource_node *)hw_mgr_res->rsrc_node;
	if (!isp_rsrc_node) {
		CAM_ERR(CAM_CUSTOM, "Invalid args");
		return -EINVAL;
	}

	hw_intf = isp_rsrc_node->hw_intf;
	if (hw_intf->hw_ops.release) {
		CAM_DBG(CAM_CUSTOM, "RELEASE HW for res_id:%u",
			hw_mgr_res->res_id);
		rc = hw_intf->hw_ops.release(hw_intf->hw_priv,
			isp_rsrc_node, sizeof(struct cam_isp_resource_node));
		if (rc)
			CAM_ERR(CAM_CUSTOM,
				"Release HW failed for hw_idx %d",
				hw_intf->hw_idx);
	}

	/* caller should make sure the resource is in a list */
	list_del_init(&hw_mgr_res->list);
	memset(hw_mgr_res, 0, sizeof(*hw_mgr_res));
	INIT_LIST_HEAD(&hw_mgr_res->list);

	return 0;
}

static int cam_custom_hw_mgr_release_hw_for_ctx(
	struct cam_custom_hw_mgr_ctx *custom_ctx)
{
	int rc = -1;
	struct cam_custom_hw_mgr_res     *hw_mgr_res;
	struct cam_custom_hw_mgr_res	 *hw_mgr_res_temp;

	/* Release custom cid */
	list_for_each_entry_safe(hw_mgr_res, hw_mgr_res_temp,
		&custom_ctx->res_list_custom_cid, list) {
		rc = cam_custom_hw_mgr_free_hw_res(hw_mgr_res);
		if (rc)
			CAM_ERR(CAM_ISP, "Can not release CID(id :%d)",
				hw_mgr_res->res_id);
		cam_custom_hw_mgr_put_res(
			&custom_ctx->free_res_list, &hw_mgr_res);
	}

	/* Release custom csid */
	list_for_each_entry_safe(hw_mgr_res, hw_mgr_res_temp,
		&custom_ctx->res_list_custom_csid, list) {
		rc = cam_custom_hw_mgr_free_hw_res(hw_mgr_res);
		if (rc)
			CAM_ERR(CAM_ISP, "Can not release CSID(id :%d)",
				hw_mgr_res->res_id);
		cam_custom_hw_mgr_put_res(
			&custom_ctx->free_res_list, &hw_mgr_res);
	}

	/* Release custom HW Here */

	return 0;
}
static int cam_custom_mgr_release_hw(void *hw_mgr_priv,
	void *release_hw_args)
{
	int                               rc           = 0;
	struct cam_hw_release_args       *release_args = release_hw_args;
	struct cam_custom_hw_mgr_ctx     *custom_ctx;

	if (!hw_mgr_priv || !release_hw_args) {
		CAM_ERR(CAM_CUSTOM, "Invalid arguments");
		return -EINVAL;
	}

	custom_ctx =
		(struct cam_custom_hw_mgr_ctx *)release_args->ctxt_to_hw_map;
	if (!custom_ctx || !custom_ctx->ctx_in_use) {
		CAM_ERR(CAM_CUSTOM, "Invalid context is used");
		return -EPERM;
	}

	CAM_DBG(CAM_CUSTOM, "Enter...ctx id:%d",
		custom_ctx->ctx_index);

	cam_custom_hw_mgr_release_hw_for_ctx(custom_ctx);
	list_del_init(&custom_ctx->list);
	custom_ctx->ctx_in_use = 0;
	cam_custom_hw_mgr_put_ctx(&g_custom_hw_mgr.free_ctx_list, &custom_ctx);
	CAM_DBG(CAM_CUSTOM, "Release Exit..");
	return rc;
}

static void cam_custom_hw_mgr_acquire_get_unified_dev_str(
	struct cam_custom_in_port_info *in,
	struct cam_isp_in_port_generic_info *gen_port_info)
{
	int i;

	gen_port_info->res_type        =  in->res_type +
		CAM_ISP_IFE_IN_RES_BASE - CAM_CUSTOM_IN_RES_BASE;
	gen_port_info->lane_type       =  in->lane_type;
	gen_port_info->lane_num        =  in->lane_num;
	gen_port_info->lane_cfg        =  in->lane_cfg;
	gen_port_info->vc[0]           =  in->vc[0];
	gen_port_info->dt[0]           =  in->dt[0];
	gen_port_info->num_valid_vc_dt =  in->num_valid_vc_dt;
	gen_port_info->format          =  in->format;
	gen_port_info->test_pattern    =  in->test_pattern;
	gen_port_info->usage_type      =  in->usage_type;
	gen_port_info->left_start      =  in->left_start;
	gen_port_info->left_stop       =  in->left_stop;
	gen_port_info->left_width      =  in->left_width;
	gen_port_info->right_start     =  in->right_start;
	gen_port_info->right_stop      =  in->right_stop;
	gen_port_info->right_width     =  in->right_width;
	gen_port_info->line_start      =  in->line_start;
	gen_port_info->line_stop       =  in->line_stop;
	gen_port_info->height          =  in->height;
	gen_port_info->pixel_clk       =  in->pixel_clk;
	gen_port_info->cust_node       =  1;
	gen_port_info->num_out_res     =  in->num_out_res;
	gen_port_info->num_bytes_out   =  in->num_bytes_out;

	for (i = 0; i < in->num_out_res; i++) {
		gen_port_info->data[i].res_type     = in->data[i].res_type;
		gen_port_info->data[i].format       = in->data[i].format;
	}
}

static int cam_custom_mgr_acquire_hw_for_ctx(
	struct cam_custom_hw_mgr_ctx           *custom_ctx,
	struct cam_isp_in_port_generic_info    *in_port_info,
	uint32_t *acquired_hw_id, uint32_t *acquired_hw_path)
{
	int rc = 0, i = 0;
	struct cam_hw_intf            *hw_intf;
	struct cam_custom_hw_mgr      *custom_hw_mgr;
	struct cam_custom_sub_mod_acq acq;

	custom_hw_mgr = custom_ctx->hw_mgr;

	/* Acquire custom csid */
	rc = cam_custom_hw_mgr_acquire_csid_res(custom_ctx, in_port_info);
	if (rc) {
		CAM_ERR(CAM_CUSTOM, "Custom csid acquire failed rc %d");
		goto err;
	}

	/* Acquire custom hw */
	for (i = 0; i < CAM_CUSTOM_HW_SUB_MOD_MAX; i++) {
		hw_intf = custom_hw_mgr->custom_hw[i];
		if (!hw_intf)
			continue;

		rc = hw_intf->hw_ops.reserve(hw_intf->hw_priv,
			&acq, sizeof(acq));
		if (rc) {
			CAM_DBG(CAM_CUSTOM,
				"No custom resource from hw %d",
				hw_intf->hw_idx);
			continue;
		}
		/* need to be set in reserve based on HW being acquired */
		//custom_ctx->sub_hw_list[i].hw_res = acq.rsrc_node;
		//custom_ctx->sub_hw_list[i].res_type = <res_type>
		//custom_ctx->sub_hw_list[i].res_id = <res_id>;
		break;
	}

err:
	return rc;
}

static int cam_custom_mgr_acquire_hw(
	void *hw_mgr_priv,
	void *acquire_hw_args)
{
	int rc = -1;
	int32_t i;
	uint32_t                             in_port_length;
	struct cam_custom_hw_mgr_ctx        *custom_ctx;
	struct cam_custom_hw_mgr            *custom_hw_mgr;
	struct cam_hw_acquire_args          *acquire_args =
		(struct cam_hw_acquire_args *)  acquire_hw_args;
	struct cam_custom_in_port_info      *in_port_info;
	struct cam_custom_resource          *custom_rsrc;
	struct cam_isp_in_port_generic_info *gen_port_info = NULL;

	if (!hw_mgr_priv || !acquire_args || (acquire_args->num_acq <= 0)) {
		CAM_ERR(CAM_CUSTOM, "Invalid params");
		return -EINVAL;
	}

	custom_hw_mgr = (struct cam_custom_hw_mgr *) hw_mgr_priv;
	mutex_lock(&g_custom_hw_mgr.ctx_mutex);
	rc = cam_custom_hw_mgr_get_ctx(
		&custom_hw_mgr->free_ctx_list, &custom_ctx);
	if (rc || !custom_ctx) {
		CAM_ERR(CAM_CUSTOM, "Get custom hw context failed");
		mutex_unlock(&g_custom_hw_mgr.ctx_mutex);
		goto err;
	}
	mutex_unlock(&g_custom_hw_mgr.ctx_mutex);

	/* Handle Acquire Here */
	custom_ctx->hw_mgr = custom_hw_mgr;
	custom_ctx->cb_priv = acquire_args->context_data;
	custom_ctx->event_cb = acquire_args->event_cb;

	custom_rsrc = kcalloc(acquire_args->num_acq,
		sizeof(*custom_rsrc), GFP_KERNEL);
	if (!custom_rsrc) {
		rc = -ENOMEM;
		goto free_ctx;
	}

	CAM_DBG(CAM_CUSTOM, "start copy %d resources from user",
		acquire_args->num_acq);

	if (copy_from_user(custom_rsrc,
		(void __user *)acquire_args->acquire_info,
		((sizeof(*custom_rsrc)) * acquire_args->num_acq))) {
		rc = -EFAULT;
		goto free_ctx;
	}

	for (i = 0; i < acquire_args->num_acq; i++) {
		if (custom_rsrc[i].resource_id != CAM_CUSTOM_RES_ID_PORT)
			continue;

		CAM_DBG(CAM_CUSTOM, "acquire no = %d total = %d", i,
			acquire_args->num_acq);

		CAM_DBG(CAM_CUSTOM,
			"start copy from user handle %lld with len = %d",
			custom_rsrc[i].res_hdl,
			custom_rsrc[i].length);

		in_port_length = sizeof(struct cam_custom_in_port_info);
		if (in_port_length > custom_rsrc[i].length) {
			CAM_ERR(CAM_CUSTOM, "buffer size is not enough");
			rc = -EINVAL;
			goto free_res;
		}

		in_port_info = memdup_user(
			u64_to_user_ptr(custom_rsrc[i].res_hdl),
			custom_rsrc[i].length);

		if (!IS_ERR(in_port_info)) {
			if (in_port_info->num_out_res >
				CAM_CUSTOM_HW_OUT_RES_MAX) {
				CAM_ERR(CAM_CUSTOM, "too many output res %d",
					in_port_info->num_out_res);
				rc = -EINVAL;
				kfree(in_port_info);
				goto free_res;
			}

			in_port_length =
				sizeof(struct cam_custom_in_port_info) +
				(in_port_info->num_out_res - 1) *
				sizeof(struct cam_custom_out_port_info);

			if (in_port_length > custom_rsrc[i].length) {
				CAM_ERR(CAM_CUSTOM,
					"buffer size is not enough");
				rc = -EINVAL;
				kfree(in_port_info);
				goto free_res;
			}

			gen_port_info = kzalloc(
				sizeof(struct cam_isp_in_port_generic_info),
				GFP_KERNEL);
			if (gen_port_info == NULL) {
				rc = -ENOMEM;
				goto free_res;
			}

			gen_port_info->data = kcalloc(
				sizeof(struct cam_isp_out_port_generic_info),
				in_port_info->num_out_res, GFP_KERNEL);
			if (gen_port_info->data == NULL) {
				kfree(gen_port_info);
				gen_port_info = NULL;
				rc = -ENOMEM;
				goto free_res;
			}

			cam_custom_hw_mgr_acquire_get_unified_dev_str(
				in_port_info, gen_port_info);

			rc = cam_custom_mgr_acquire_hw_for_ctx(custom_ctx,
				gen_port_info, &acquire_args->acquired_hw_id[i],
				acquire_args->acquired_hw_path[i]);

			kfree(in_port_info);
			if (gen_port_info != NULL) {
				kfree(gen_port_info->data);
				kfree(gen_port_info);
				gen_port_info = NULL;
			}

			if (rc) {
				CAM_ERR(CAM_CUSTOM, "can not acquire resource");
				goto free_res;
			}
	} else {
		CAM_ERR(CAM_CUSTOM,
			"Copy from user failed with in_port = %pK",
			in_port_info);
			rc = -EFAULT;
			goto free_res;
		}
	}

	custom_ctx->ctx_in_use = 1;
	acquire_args->ctxt_to_hw_map = custom_ctx;
	CAM_DBG(CAM_CUSTOM, "Exit...(success)");
	return 0;

free_res:
	cam_custom_hw_mgr_release_hw_for_ctx(custom_ctx);
free_ctx:
	cam_custom_hw_mgr_put_ctx(&custom_hw_mgr->free_ctx_list, &custom_ctx);
err:
	CAM_DBG(CAM_CUSTOM, "Exit...(rc=%d)", rc);
	return rc;
}

static int cam_custom_add_io_buffers(
	int                                   iommu_hdl,
	struct cam_hw_prepare_update_args    *prepare)
{
	int rc = 0, i = 0;
	int32_t                             hdl;
	uint32_t                            plane_id;
	struct cam_buf_io_cfg              *io_cfg;

	io_cfg = (struct cam_buf_io_cfg *)((uint8_t *)
			&prepare->packet->payload +
			prepare->packet->io_configs_offset);

	/* Validate hw update entries */

	for (i = 0; i < prepare->packet->num_io_configs; i++) {
		CAM_DBG(CAM_CUSTOM, "======= io config idx %d ============", i);
		CAM_DBG(CAM_CUSTOM,
			"i %d req_id %llu resource_type:%d fence:%d direction %d",
			i, prepare->packet->header.request_id,
			io_cfg[i].resource_type, io_cfg[i].fence,
			io_cfg[i].direction);

		CAM_DBG(CAM_CUSTOM, "format: %d", io_cfg[i].format);

		if (io_cfg[i].direction == CAM_BUF_OUTPUT) {
			CAM_DBG(CAM_CUSTOM,
				"output fence 0x%x", io_cfg[i].fence);
		} else if (io_cfg[i].direction == CAM_BUF_INPUT) {
			CAM_DBG(CAM_CUSTOM,
				"input fence 0x%x", io_cfg[i].fence);
		} else {
			CAM_ERR(CAM_CUSTOM, "Invalid io config direction :%d",
				io_cfg[i].direction);
			return -EINVAL;
		}

		for (plane_id = 0; plane_id < CAM_PACKET_MAX_PLANES;
			plane_id++) {
			if (!io_cfg[i].mem_handle[plane_id])
				continue;

			hdl = io_cfg[i].mem_handle[plane_id];
			CAM_DBG(CAM_CUSTOM, "handle 0x%x for plane %d",
				hdl, plane_id);
			/* Use cam_mem_get_io_buf() to retrieve iova */
		}

		/* Do other I/O config operations */
	}

	return rc;
}

static int cam_custom_mgr_prepare_hw_update(void *hw_mgr_priv,
	void *prepare_hw_update_args)
{
	int rc = 0;
	struct cam_hw_prepare_update_args        *prepare;
	struct cam_cmd_buf_desc                  *cmd_desc = NULL;
	struct cam_custom_prepare_hw_update_data *prepare_hw_data;
	struct cam_custom_hw_mgr                 *hw_mgr;
	struct cam_custom_hw_mgr_ctx             *ctx = NULL;
	uint32_t                                 *ptr;
	size_t                                    len;
	struct cam_custom_cmd_buf_type_1         *custom_buf_type1;

	if (!hw_mgr_priv || !prepare_hw_update_args) {
		CAM_ERR(CAM_CUSTOM, "Invalid args");
		return -EINVAL;
	}

	hw_mgr = (struct cam_custom_hw_mgr *) hw_mgr_priv;
	prepare =
		(struct cam_hw_prepare_update_args *) prepare_hw_update_args;

	CAM_DBG(CAM_CUSTOM, "Enter for req_id %lld",
		prepare->packet->header.request_id);

	/* Prepare packet */
	prepare_hw_data =
		(struct cam_custom_prepare_hw_update_data *)prepare->priv;
	prepare_hw_data->packet_opcode_type =
		(prepare->packet->header.op_code & 0xFFF);
	ctx = (struct cam_custom_hw_mgr_ctx *) prepare->ctxt_to_hw_map;

	/* Test purposes-check the data in cmd buffer */
	cmd_desc = (struct cam_cmd_buf_desc *)
		((uint8_t *)&prepare->packet->payload +
		prepare->packet->cmd_buf_offset);
	rc = cam_packet_util_get_cmd_mem_addr(
			cmd_desc->mem_handle, &ptr, &len);
	if (!rc) {
		ptr += (cmd_desc->offset / 4);
		custom_buf_type1 =
			(struct cam_custom_cmd_buf_type_1 *)ptr;
		CAM_DBG(CAM_CUSTOM, "frame num %u",
			custom_buf_type1->custom_info);
	}

	cam_custom_add_io_buffers(hw_mgr->img_iommu_hdl, prepare);
	return 0;
}

static int cam_custom_mgr_config_hw(void *hw_mgr_priv,
	void *hw_config_args)
{
	int rc = 0;
	int i = 0;
	struct cam_custom_hw_mgr_ctx *custom_ctx;
	struct cam_custom_hw_mgr_res *res;
	struct cam_hw_config_args    *cfg;
	struct cam_hw_intf           *hw_intf = NULL;

	CAM_DBG(CAM_CUSTOM, "Enter");
	if (!hw_mgr_priv || !hw_config_args) {
		CAM_ERR(CAM_CUSTOM, "Invalid arguments");
		return -EINVAL;
	}

	cfg =
		(struct cam_hw_config_args *)hw_config_args;
	custom_ctx = cfg->ctxt_to_hw_map;

	if (!custom_ctx->ctx_in_use) {
		CAM_ERR(CAM_CUSTOM, "Invalid context parameters");
		return -EPERM;
	}

	for (i = 0; i < CAM_CUSTOM_HW_SUB_MOD_MAX; i++) {
		res = &custom_ctx->sub_hw_list[i];
		if (res->hw_res) {
			hw_intf = res->hw_res->hw_intf;
			if (hw_intf->hw_ops.process_cmd) {
				struct cam_custom_sub_mod_req_to_dev req_to_dev;

				req_to_dev.ctx_idx = custom_ctx->ctx_index;
				req_to_dev.dev_idx = i;
				req_to_dev.req_id = cfg->request_id;
				rc = hw_intf->hw_ops.process_cmd(
					hw_intf->hw_priv,
					CAM_CUSTOM_SUBMIT_REQ,
					&req_to_dev, sizeof(req_to_dev));
			}
		}
	}

	return rc;
}

static int cam_custom_hw_mgr_irq_cb(void *data,
	struct cam_custom_hw_cb_args *cb_args)
{
	struct cam_custom_sub_mod_req_to_dev *proc_req;
	struct cam_hw_done_event_data         evt_data;
	struct cam_custom_hw_mgr_ctx         *custom_ctx;
	uint32_t ctx_idx;

	proc_req = cb_args->req_info;
	ctx_idx = proc_req->ctx_idx;
	custom_ctx = &g_custom_hw_mgr.ctx_pool[ctx_idx];

	if (!custom_ctx->ctx_in_use) {
		CAM_ERR(CAM_CUSTOM, "ctx %u not in use", ctx_idx);
		return 0;
	}

	/* Based on irq status notify success/failure */

	evt_data.request_id = proc_req->req_id;
	custom_ctx->event_cb(custom_ctx->cb_priv,
		CAM_CUSTOM_EVENT_BUF_DONE, &evt_data);

	return 0;
}

int cam_custom_hw_mgr_init(struct device_node *of_node,
	struct cam_hw_mgr_intf *hw_mgr_intf, int *iommu_hdl)
{
	int rc = 0;
	int i, j;
	struct cam_custom_hw_mgr_ctx *ctx_pool;
	struct cam_custom_sub_mod_set_irq_cb irq_cb_args;
	struct cam_hw_intf *hw_intf = NULL;

	memset(&g_custom_hw_mgr, 0, sizeof(g_custom_hw_mgr));
	mutex_init(&g_custom_hw_mgr.ctx_mutex);

	/* fill custom hw intf information */
	for (i = 0; i < CAM_CUSTOM_HW_SUB_MOD_MAX; i++) {
		/* Initialize sub modules */
		rc = cam_custom_hw_sub_mod_init(
				&g_custom_hw_mgr.custom_hw[i], i);

		/* handle in case init fails */
		if (g_custom_hw_mgr.custom_hw[i]) {
			hw_intf = g_custom_hw_mgr.custom_hw[i];
			if (hw_intf->hw_ops.process_cmd) {
				irq_cb_args.custom_hw_mgr_cb =
					cam_custom_hw_mgr_irq_cb;
				irq_cb_args.data =
					g_custom_hw_mgr.custom_hw[i]->hw_priv;
				hw_intf->hw_ops.process_cmd(hw_intf->hw_priv,
					CAM_CUSTOM_SET_IRQ_CB, &irq_cb_args,
					sizeof(irq_cb_args));
			}
		}
	}

	for (i = 0; i < CAM_CUSTOM_CSID_HW_MAX; i++) {
		/* Initialize csid custom modules */
		rc = cam_custom_csid_hw_init(
			&g_custom_hw_mgr.csid_devices[i], i);
	}

	INIT_LIST_HEAD(&g_custom_hw_mgr.free_ctx_list);
	INIT_LIST_HEAD(&g_custom_hw_mgr.used_ctx_list);

	/*
	 *  for now, we only support one iommu handle. later
	 *  we will need to setup more iommu handle for other
	 *  use cases.
	 *  Also, we have to release them once we have the
	 *  deinit support
	 */
	if (cam_smmu_get_handle("custom",
		&g_custom_hw_mgr.img_iommu_hdl)) {
		CAM_ERR(CAM_CUSTOM, "Can not get iommu handle");
		return -EINVAL;
	}

	for (i = 0; i < CAM_CTX_MAX; i++) {
		memset(&g_custom_hw_mgr.ctx_pool[i], 0,
			sizeof(g_custom_hw_mgr.ctx_pool[i]));
		INIT_LIST_HEAD(&g_custom_hw_mgr.ctx_pool[i].list);

		ctx_pool = &g_custom_hw_mgr.ctx_pool[i];

		/* init context pool */
		INIT_LIST_HEAD(&g_custom_hw_mgr.ctx_pool[i].free_res_list);
		INIT_LIST_HEAD(
			&g_custom_hw_mgr.ctx_pool[i].res_list_custom_csid);
		INIT_LIST_HEAD(
			&g_custom_hw_mgr.ctx_pool[i].res_list_custom_cid);
		for (j = 0; j < CAM_CUSTOM_HW_RES_MAX; j++) {
			INIT_LIST_HEAD(
				&g_custom_hw_mgr.ctx_pool[i].res_pool[j].list);
			list_add_tail(
				&g_custom_hw_mgr.ctx_pool[i].res_pool[j].list,
				&g_custom_hw_mgr.ctx_pool[i].free_res_list);
		}

		g_custom_hw_mgr.ctx_pool[i].ctx_index = i;
		g_custom_hw_mgr.ctx_pool[i].hw_mgr = &g_custom_hw_mgr;

		list_add_tail(&g_custom_hw_mgr.ctx_pool[i].list,
			&g_custom_hw_mgr.free_ctx_list);
	}

	/* fill return structure */
	hw_mgr_intf->hw_mgr_priv = &g_custom_hw_mgr;
	hw_mgr_intf->hw_get_caps = cam_custom_mgr_get_hw_caps;
	hw_mgr_intf->hw_acquire = cam_custom_mgr_acquire_hw;
	hw_mgr_intf->hw_start = cam_custom_mgr_start_hw;
	hw_mgr_intf->hw_stop = cam_custom_mgr_stop_hw;
	hw_mgr_intf->hw_read = cam_custom_mgr_read;
	hw_mgr_intf->hw_write = cam_custom_mgr_write;
	hw_mgr_intf->hw_release = cam_custom_mgr_release_hw;
	hw_mgr_intf->hw_prepare_update = cam_custom_mgr_prepare_hw_update;
	hw_mgr_intf->hw_config = cam_custom_mgr_config_hw;

	if (iommu_hdl)
		*iommu_hdl = g_custom_hw_mgr.img_iommu_hdl;

	CAM_DBG(CAM_CUSTOM, "HW manager initialized");
	return 0;
}
