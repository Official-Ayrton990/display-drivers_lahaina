/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s:%d: " fmt, __func__, __LINE__
#include <linux/backlight.h>
#include <linux/of_gpio.h>
#include <linux/pwm.h>
#include <linux/sysfs.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <video/mipi_display.h>

#include "dsi_panel.h"

#define BL_NODE_NAME_SIZE 32

#define BL_STATE_LP		(1 << 31)
#define BL_STATE_LP2		(1 << 30)

struct dsi_backlight_pwm_config {
	struct pwm_device *pwm_bl;
	bool pwm_enabled;
	u32 pwm_period_usecs;
};

static inline bool is_lp_mode(unsigned long state)
{
	return (state & (BL_STATE_LP | BL_STATE_LP2)) != 0;
}

static int dsi_panel_pwm_bl_register(struct dsi_backlight_config *bl);

static void dsi_panel_bl_free_unregister(struct dsi_backlight_config *bl)
{
	kfree(bl->priv);
}

static int dsi_backlight_update_dcs(struct dsi_backlight_config *bl, u32 bl_lvl)
{
	int rc = 0;
	struct dsi_panel *panel;
	struct mipi_dsi_device *dsi;
	size_t num_params;

	if (!bl || (bl_lvl > 0xffff)) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	panel = container_of(bl, struct dsi_panel, bl_config);
	/* if no change in backlight, abort */
	if (bl_lvl == bl->bl_actual)
		return 0;

	dsi = &panel->mipi_device;

	num_params = bl->bl_max_level > 0xFF ? 2 : 1;
	rc = mipi_dsi_dcs_set_display_brightness(dsi, bl_lvl, num_params);
	if (rc < 0)
		pr_err("failed to update dcs backlight:%d\n", bl_lvl);

	return rc;
}

static u32 dsi_backlight_calculate(struct dsi_backlight_config *bl,
				   int brightness)
{
	int bl_lvl = 0;

	if (brightness) {
		int bl_min = bl->bl_min_level ? : 1;
		int bl_range = bl->bl_max_level - bl_min;
		int bl_temp;

		/* scale backlight */
		bl_temp = mult_frac(brightness, bl->bl_scale,
				    MAX_BL_SCALE_LEVEL);

		bl_temp = mult_frac(bl_temp, bl->bl_scale_sv,
				    MAX_SV_BL_SCALE_LEVEL);


		/* map UI brightness into driver backlight level rounding it */
		if (bl_temp > 1)
			bl_lvl = DIV_ROUND_CLOSEST((bl_temp - 1) * bl_range,
					bl->brightness_max_level - 1);
		bl_lvl += bl_min;

		pr_debug("brightness=%d, bl_scale=%d, sv=%d, bl_lvl=%d\n",
			 brightness, bl->bl_scale,
			 bl->bl_scale_sv, bl_lvl);
	}

	return bl_lvl;
}

static int dsi_backlight_update_status(struct backlight_device *bd)
{
	struct dsi_backlight_config *bl = bl_get_data(bd);
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	int brightness = bd->props.brightness;
	int bl_lvl;
	int rc = 0;

	if ((bd->props.state & (BL_CORE_FBBLANK | BL_CORE_SUSPENDED)) ||
			(bd->props.power != FB_BLANK_UNBLANK))
		brightness = 0;

	bl_lvl = dsi_backlight_calculate(bl, brightness);
	if (bl_lvl == bl->bl_actual && bl->last_state == bd->props.state)
		return 0;

	mutex_lock(&panel->panel_lock);

	if (!bl->allow_bl_update) {
		bl->bl_update_pending = true;
		goto done;
	}

	if (dsi_panel_initialized(panel) && bl->update_bl) {
		pr_info("req:%d bl:%d state:0x%x\n",
			bd->props.brightness, bl_lvl, bd->props.state);

		rc = bl->update_bl(bl, bl_lvl);
		if (rc) {
			pr_err("unable to set backlight (%d)\n", rc);
			goto done;
		}
		bl->bl_update_pending = false;
	}
	bl->bl_actual = bl_lvl;
	bl->last_state = bd->props.state;

done:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_backlight_get_brightness(struct backlight_device *bd)
{
	struct dsi_backlight_config *bl = bl_get_data(bd);

	return bl->bl_actual;
}

static const struct backlight_ops dsi_backlight_ops = {
	.update_status = dsi_backlight_update_status,
	.get_brightness = dsi_backlight_get_brightness,
};

static ssize_t alpm_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct dsi_backlight_config *bl = bl_get_data(bd);
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	int rc, alpm_mode;
	const unsigned int lp_state = bl->bl_device->props.state &
			(BL_STATE_LP | BL_STATE_LP2);

	rc = kstrtoint(buf, 0, &alpm_mode);
	if (rc)
		return rc;

	if (bl->bl_device->props.state & BL_CORE_FBBLANK) {
		return -EINVAL;
	} else if ((alpm_mode == 1) && (lp_state != BL_STATE_LP)) {
		pr_info("activating lp1 mode\n");
		dsi_panel_set_lp1(panel);
	} else if ((alpm_mode > 1) && !(lp_state & BL_STATE_LP2)) {
		pr_info("activating lp2 mode\n");
		dsi_panel_set_lp2(panel);
	} else if (!alpm_mode && lp_state) {
		pr_info("activating normal mode\n");
		dsi_panel_set_nolp(panel);
	}

	return count;
}

static ssize_t alpm_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	int alpm_mode;

	if (bd->props.state & BL_STATE_LP2)
		alpm_mode = 2;
	else
		alpm_mode = (bd->props.state & BL_STATE_LP) != 0;

	return sprintf(buf, "%d\n", alpm_mode);
}
static DEVICE_ATTR_RW(alpm_mode);

static struct attribute *bl_device_attrs[] = {
	&dev_attr_alpm_mode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bl_device);

static int dsi_backlight_register(struct dsi_backlight_config *bl)
{
	static int display_count;
	char bl_node_name[BL_NODE_NAME_SIZE];
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.power = FB_BLANK_UNBLANK,
	};
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct regulator *reg;

	props.max_brightness = bl->brightness_max_level;
	props.brightness = bl->brightness_max_level / 2;

	snprintf(bl_node_name, BL_NODE_NAME_SIZE, "panel%u-backlight",
		 display_count);
	bl->bl_device = devm_backlight_device_register(panel->parent,
				bl_node_name, panel->parent, bl,
				&dsi_backlight_ops, &props);
	if (IS_ERR_OR_NULL(bl->bl_device)) {
		bl->bl_device = NULL;
		return -ENODEV;
	}

	if (sysfs_create_groups(&bl->bl_device->dev.kobj, bl_device_groups))
		pr_warn("unable to create device groups\n");

	reg = regulator_get_optional(panel->parent, "lab");
	if (!PTR_ERR_OR_ZERO(reg)) {
		pr_info("LAB regulator found\n");
		panel->bl_config.lab_vreg = reg;
	}

	display_count++;
	return 0;
}

static unsigned long get_state_after_dpms(struct dsi_backlight_config *bl,
				   int power_mode)
{
	struct backlight_device *bd = bl->bl_device;
	unsigned long state = bd->props.state;

	switch (power_mode) {
	case SDE_MODE_DPMS_ON:
		state &= ~(BL_CORE_FBBLANK | BL_STATE_LP | BL_STATE_LP2);
		break;
	case SDE_MODE_DPMS_OFF:
		state &= ~(BL_STATE_LP | BL_STATE_LP2);
		state |= BL_CORE_FBBLANK;
		break;
	case SDE_MODE_DPMS_LP1:
		state |= BL_STATE_LP;
		state &= ~BL_STATE_LP2;
		break;
	case SDE_MODE_DPMS_LP2:
		state |= BL_STATE_LP | BL_STATE_LP2;
		break;
	}

	return state;
}

int dsi_backlight_early_dpms(struct dsi_backlight_config *bl, int power_mode)
{
	struct backlight_device *bd = bl->bl_device;
	unsigned long state;

	if (!bd)
		return 0;

	pr_info("power_mode:%d state:0x%0x\n", power_mode, bd->props.state);

	mutex_lock(&bd->ops_lock);
	state = get_state_after_dpms(bl, power_mode);

	if (bl->lab_vreg) {
		if (is_lp_mode(bl->last_state) && !is_lp_mode(state)) {
			/* LP -> no LP */
			pr_debug("enabling lab vreg\n");
			regulator_set_mode(bl->lab_vreg, REGULATOR_MODE_NORMAL);
		} else if (!is_lp_mode(bl->last_state) && is_lp_mode(state)) {
			/* no LP -> LP */
			pr_debug("disabling lab vreg\n");
			regulator_set_mode(bl->lab_vreg, REGULATOR_MODE_IDLE);
		}
	}
	mutex_unlock(&bd->ops_lock);

	return 0;
}

int dsi_backlight_late_dpms(struct dsi_backlight_config *bl, int power_mode)
{
	struct backlight_device *bd = bl->bl_device;
	unsigned long state;

	if (!bd)
		return 0;

	pr_debug("power_mode:%d state:0x%0x\n", power_mode, bd->props.state);

	mutex_lock(&bd->ops_lock);
	state = get_state_after_dpms(bl, power_mode);

	bd->props.power = state & BL_CORE_FBBLANK ? FB_BLANK_POWERDOWN :
			FB_BLANK_UNBLANK;
	bd->props.state = state;

	backlight_update_status(bd);
	mutex_unlock(&bd->ops_lock);

	return 0;
}

#define MAX_BINNED_BL_MODES 10

struct binned_lp_node {
	struct list_head head;
	const char *name;
	u32 bl_threshold;
	struct dsi_panel_cmd_set dsi_cmd;
};

struct binned_lp_data {
	struct list_head mode_list;
	struct binned_lp_node *last_lp_mode;
	struct binned_lp_node priv_pool[MAX_BINNED_BL_MODES];
};

static int dsi_panel_binned_bl_update(struct dsi_backlight_config *bl,
				      u32 bl_lvl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct binned_lp_data *lp_data = bl->priv;
	struct binned_lp_node *node = NULL;
	struct backlight_properties *props = &bl->bl_device->props;

	if (is_lp_mode(props->state)) {
		struct binned_lp_node *tmp;

		list_for_each_entry(tmp, &lp_data->mode_list, head) {
			if (props->brightness <= tmp->bl_threshold) {
				node = tmp;
				break;
			}
		}
		WARN(!node, "unable to find lp node for bl_lvl: %d\n",
		     props->brightness);
	}

	if (node != lp_data->last_lp_mode) {
		lp_data->last_lp_mode = node;
		if (node) {
			pr_debug("switching display lp mode: %s (%d)\n",
				node->name, props->brightness);
			dsi_panel_cmd_set_transfer(panel, &node->dsi_cmd);
		} else {
			/* ensure update after lpm */
			bl->bl_actual = -1;
		}
	}

	/* no need to send backlight command if HLPM active */
	if (node)
		return 0;

	return dsi_backlight_update_dcs(bl, bl_lvl);
}

static int _dsi_panel_binned_lp_parse(struct device_node *np,
				      struct binned_lp_node *node)
{
	int rc;
	u32 val = 0;

	rc = of_property_read_u32(np, "google,dsi-lp-brightness-threshold",
				  &val);
	/* treat lack of property as max threshold */
	node->bl_threshold = !rc ? val : UINT_MAX;

	rc = dsi_panel_parse_dt_cmd_set(np, "google,dsi-lp-command",
					"google,dsi-lp-command-state",
					&node->dsi_cmd);
	if (rc) {
		pr_err("Unable to parse dsi-lp-command\n");
		return rc;
	}

	of_property_read_string(np, "label", &node->name);

	pr_debug("Successfully parsed lp mode: %s threshold: %d\n",
		node->name, node->bl_threshold);

	return 0;
}

static int _dsi_panel_binned_bl_cmp(void *priv, struct list_head *lha,
				    struct list_head *lhb)
{
	struct binned_lp_node *a = list_entry(lha, struct binned_lp_node, head);
	struct binned_lp_node *b = list_entry(lhb, struct binned_lp_node, head);

	return a->bl_threshold - b->bl_threshold;
}

static int dsi_panel_binned_lp_register(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct binned_lp_data *lp_data;
	struct device_node *lp_modes_np, *child_np;
	struct binned_lp_node *lp_node;
	int num_modes;
	int rc = -ENOTSUPP;

	lp_data = kzalloc(sizeof(*lp_data), GFP_KERNEL);
	if (!lp_data)
		return -ENOMEM;

	lp_modes_np = of_get_child_by_name(panel->panel_of_node,
					   "google,lp-modes");

	if (!lp_modes_np) {
		kfree(lp_data);
		return rc;
	}

	num_modes = of_get_child_count(lp_modes_np);
	if (!num_modes || (num_modes > MAX_BINNED_BL_MODES)) {
		pr_err("Invalid binned brightness modes: %d\n", num_modes);
		goto exit;
	}

	INIT_LIST_HEAD(&lp_data->mode_list);
	lp_node = lp_data->priv_pool;

	for_each_child_of_node(lp_modes_np, child_np) {
		rc = _dsi_panel_binned_lp_parse(child_np, lp_node);
		if (rc)
			goto exit;

		list_add_tail(&lp_node->head, &lp_data->mode_list);
		lp_node++;
		if (lp_node > &lp_data->priv_pool[MAX_BINNED_BL_MODES - 1]) {
			pr_err("Too many LP modes\n");
			rc = -ENOTSUPP;
			goto exit;
		}
	}
	list_sort(NULL, &lp_data->mode_list, _dsi_panel_binned_bl_cmp);

	bl->update_bl = dsi_panel_binned_bl_update;
	bl->unregister = dsi_panel_bl_free_unregister;
	bl->priv = lp_data;

exit:
	of_node_put(lp_modes_np);
	if (rc)
		kfree(lp_data);

	return rc;
}

static const struct of_device_id dsi_backlight_dt_match[] = {
	{
		.compatible = "google,dsi_binned_lp",
		.data = dsi_panel_binned_lp_register,
	},
	{}
};

int dsi_panel_bl_register(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_backlight_config *bl = &panel->bl_config;
	const struct of_device_id *match;
	int (*register_func)(struct dsi_backlight_config *) = NULL;

	match = of_match_node(dsi_backlight_dt_match, panel->panel_of_node);
	if (match && match->data) {
		register_func = match->data;
		rc = register_func(bl);
	}

	if (!register_func || (rc == -ENOTSUPP)) {
		switch (bl->type) {
		case DSI_BACKLIGHT_WLED:
			break;
		case DSI_BACKLIGHT_DCS:
			bl->update_bl = dsi_backlight_update_dcs;
			break;
		case DSI_BACKLIGHT_PWM:
			register_func = dsi_panel_pwm_bl_register;
			break;
		default:
			pr_err("Backlight type(%d) not supported\n", bl->type);
			rc = -ENOTSUPP;
			break;
		}

		if (register_func)
			rc = register_func(bl);
	}

	if (!rc)
		rc = dsi_backlight_register(bl);

	return rc;
}


int dsi_panel_bl_unregister(struct dsi_panel *panel)
{
	struct dsi_backlight_config *bl = &panel->bl_config;

	if (bl->unregister)
		bl->unregister(bl);

	if (bl->bl_device)
		sysfs_remove_groups(&bl->bl_device->dev.kobj, bl_device_groups);

	return 0;
}

static int dsi_panel_bl_parse_pwm_config(struct dsi_panel *panel,
				struct dsi_backlight_pwm_config *config)
{
	int rc = 0;
	u32 val;
	struct dsi_parser_utils *utils = &panel->utils;

	rc = utils->read_u32(utils->data, "qcom,bl-pmic-pwm-period-usecs",
				  &val);
	if (rc) {
		pr_err("bl-pmic-pwm-period-usecs is not defined, rc=%d\n", rc);
		goto error;
	}
	config->pwm_period_usecs = val;

error:
	return rc;
}

static void dsi_panel_pwm_bl_unregister(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct dsi_backlight_pwm_config *pwm_cfg = bl->priv;

	devm_pwm_put(panel->parent, pwm_cfg->pwm_bl);
	kfree(pwm_cfg);
}

static int dsi_panel_pwm_bl_register(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct dsi_backlight_pwm_config *pwm_cfg;
	int rc = 0;

	pwm_cfg = kzalloc(sizeof(*pwm_cfg), GFP_KERNEL);
	if (!pwm_cfg)
		return -ENOMEM;

	pwm_cfg->pwm_bl = devm_of_pwm_get(panel->parent, panel->panel_of_node, NULL);
	if (IS_ERR_OR_NULL(pwm_cfg->pwm_bl)) {
		rc = PTR_ERR(pwm_cfg->pwm_bl);
		pr_err("[%s] failed to request pwm, rc=%d\n", panel->name,
			rc);
		kfree(pwm_cfg);
		return rc;
	}

	rc = dsi_panel_bl_parse_pwm_config(panel, pwm_cfg);
	if (rc) {
		pr_err("[%s] failed to parse pwm config, rc=%d\n",
		       panel->name, rc);
		dsi_panel_pwm_bl_unregister(bl);
		return rc;
	}

	bl->priv = pwm_cfg;
	bl->unregister = dsi_panel_pwm_bl_unregister;

	return 0;
}

int dsi_panel_bl_parse_config(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	int rc = 0;
	u32 val = 0;
	const char *bl_type;
	const char *data;
	struct dsi_parser_utils *utils = &panel->utils;
	char *bl_name;

	if (!strcmp(panel->type, "primary"))
		bl_name = "qcom,mdss-dsi-bl-pmic-control-type";
	else
		bl_name = "qcom,mdss-dsi-sec-bl-pmic-control-type";

	bl_type = utils->get_property(utils->data, bl_name, NULL);
	if (!bl_type) {
		bl->type = DSI_BACKLIGHT_UNKNOWN;
	} else if (!strcmp(bl_type, "bl_ctrl_pwm")) {
		bl->type = DSI_BACKLIGHT_PWM;
	} else if (!strcmp(bl_type, "bl_ctrl_wled")) {
		bl->type = DSI_BACKLIGHT_WLED;
	} else if (!strcmp(bl_type, "bl_ctrl_dcs")) {
		bl->type = DSI_BACKLIGHT_DCS;
	} else if (!strcmp(bl_type, "bl_ctrl_external")) {
		bl->type = DSI_BACKLIGHT_EXTERNAL;
	} else {
		pr_debug("[%s] bl-pmic-control-type unknown-%s\n",
			 panel->name, bl_type);
		bl->type = DSI_BACKLIGHT_UNKNOWN;
	}
	data = utils->get_property(utils->data, "qcom,bl-update-flag", NULL);
	if (!data) {
		panel->bl_config.bl_update = BL_UPDATE_NONE;
	} else if (!strcmp(data, "delay_until_first_frame")) {
		panel->bl_config.bl_update = BL_UPDATE_DELAY_UNTIL_FIRST_FRAME;
	} else {
		pr_debug("[%s] No valid bl-update-flag: %s\n",
						panel->name, data);
		panel->bl_config.bl_update = BL_UPDATE_NONE;
	}
	panel->bl_config.bl_scale = MAX_BL_SCALE_LEVEL;
	panel->bl_config.bl_scale_sv = MAX_SV_BL_SCALE_LEVEL;
	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-bl-min-level", &val);
	if (rc) {
		pr_debug("[%s] bl-min-level unspecified, defaulting to zero\n",
			 panel->name);
		bl->bl_min_level = 0;
	} else {
		bl->bl_min_level = val;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-bl-max-level", &val);
	if (rc) {
		pr_debug("[%s] bl-max-level unspecified, defaulting to max level\n",
			 panel->name);
		bl->bl_max_level = MAX_BL_LEVEL;
	} else {
		bl->bl_max_level = val;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-brightness-max-level",
		&val);
	if (rc) {
		pr_debug("[%s] brigheness-max-level unspecified, defaulting to 255\n",
			 panel->name);
		bl->brightness_max_level = 255;
	} else {
		bl->brightness_max_level = val;
	}

	bl->en_gpio = utils->get_named_gpio(utils->data,
					      "qcom,platform-bklight-en-gpio",
					      0);
	if (!gpio_is_valid(bl->en_gpio)) {
		if (bl->en_gpio == -EPROBE_DEFER) {
			pr_debug("[%s] failed to get bklt gpio, rc=%d\n",
					panel->name, rc);
			rc = -EPROBE_DEFER;
			goto error;
		} else {
			pr_debug("[%s] failed to get bklt gpio, rc=%d\n",
					 panel->name, rc);
			rc = 0;
			goto error;
		}
	}

error:
	return rc;
}
