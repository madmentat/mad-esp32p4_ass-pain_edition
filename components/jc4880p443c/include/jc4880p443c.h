/*
 * SPDX-FileCopyrightText: 2024 Your Name
 *
 * SPDX-License-Identifier: MIT
 * 
 * JC4880P443C - ST7701 480x800 MIPI-DSI Display Module
 * 
 * This component provides the validated initialization sequence
 * for the AliExpress JC4880P443C display module.
 */

#pragma once

#include <stdint.h>
#include "esp_lcd_st7701.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the ST7701 initialization commands for JC4880P443C display
 * 
 * @param[out] out_cmds Pointer to store the commands array
 * @param[out] out_size Pointer to store the number of commands
 */
void jc4880p443c_get_init_cmds(const st7701_lcd_init_cmd_t **out_cmds, uint16_t *out_size);

/**
 * @brief Get the JC4880P443C display resolution
 * 
 * @param[out] h_res Horizontal resolution (480)
 * @param[out] v_res Vertical resolution (800)
 */
void jc4880p443c_get_resolution(uint16_t *h_res, uint16_t *v_res);

/**
 * @brief Get recommended MIPI DSI configuration for JC4880P443C
 * 
 * @param[out] lane_bit_rate_mbps Lane bit rate in Mbps (750)
 * @param[out] num_lanes Number of data lanes (2)
 */
void jc4880p443c_get_dsi_config(uint32_t *lane_bit_rate_mbps, uint8_t *num_lanes);

/**
 * @brief Get recommended DPI timing configuration for JC4880P443C
 * 
 * @param[out] pclk_mhz Pixel clock in MHz (34)
 * @param[out] hbp Horizontal back porch (42)
 * @param[out] hfp Horizontal front porch (42)
 * @param[out] vbp Vertical back porch (8)
 * @param[out] vfp Vertical front porch (166)
 */
void jc4880p443c_get_timing(uint32_t *pclk_mhz, 
                            uint16_t *hbp, uint16_t *hfp,
                            uint16_t *vbp, uint16_t *vfp);

#ifdef __cplusplus
}
#endif