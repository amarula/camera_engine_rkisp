/*
 * aiq_handler.cpp - AIQ handler
 *
 *  Copyright (c) 2012-2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Wind Yuan <feng.yuan@intel.com>
 * Author: Yan Zhang <yan.y.zhang@intel.com>
 */

#include "rkiq_handler.h"
#include "x3a_isp_config.h"

#include <string.h>
#include <math.h>

#define MAX_STATISTICS_WIDTH 150
#define MAX_STATISTICS_HEIGHT 150

//#define USE_RGBS_GRID_WEIGHTING
#define USE_HIST_GRID_WEIGHTING

namespace XCam {

struct IspInputParameters {

    IspInputParameters ()
    {}
};

static double
_calculate_new_value_by_speed (double start, double end, double speed)
{
    XCAM_ASSERT (speed >= 0.0 && speed <= 1.0);
    static const double value_equal_range = 0.000001;

    if (fabs (end - start) <= value_equal_range)
        return end;
    return (start * (1.0 - speed) + end * speed);
}

static double
_imx185_sensor_gain_code_to_mutiplier (uint32_t code)
{
    /* 185 sensor code : DB = 160 : 48 */
    double db;
    db = code * 48.0 / 160.0;
    return pow (10.0, db / 20.0);
}

static uint32_t
_mutiplier_to_imx185_sensor_gain_code (double mutiplier)
{
    double db = log10 (mutiplier) * 20;
    if (db > 48)
        db = 48;
    return (uint32_t) (db * 160 / 48);
}

static uint32_t
_time_to_coarse_line (const ia_aiq_exposure_sensor_descriptor *desc, uint32_t time_us)
{
    float value =  time_us * desc->pixel_clock_freq_mhz;

    value = (value + desc->pixel_periods_per_line / 2) / desc->pixel_periods_per_line;
    return (uint32_t)(value);
}

static uint32_t
_coarse_line_to_time (const ia_aiq_exposure_sensor_descriptor *desc, uint32_t coarse_line)
{
    return coarse_line * desc->pixel_periods_per_line / desc->pixel_clock_freq_mhz;
}

AiqAeHandler::AiqAeResult::AiqAeResult()
{
    xcam_mem_clear (ae_result);
    xcam_mem_clear (ae_exp_ret);
    xcam_mem_clear (aiq_exp_param);
    xcam_mem_clear (sensor_exp_param);
    xcam_mem_clear (weight_grid);
    xcam_mem_clear (flash_param);
}

void
AiqAeHandler::AiqAeResult::copy (ia_aiq_ae_results *result)
{
    XCAM_ASSERT (result);

    this->ae_result = *result;
    this->aiq_exp_param = *result->exposures[0].exposure;
    this->sensor_exp_param = *result->exposures[0].sensor_exposure;
    this->weight_grid = *result->weight_grid;

    this->ae_exp_ret.exposure = &this->aiq_exp_param;
    this->ae_exp_ret.sensor_exposure = &this->sensor_exp_param;
    this->ae_result.exposures = &this->ae_exp_ret;
    this->ae_result.weight_grid = &this->weight_grid;

    this->ae_result.num_exposures = 1;
}

AiqAeHandler::AiqAeHandler (X3aAnalyzerRKiq *analyzer, SmartPtr<RKiqCompositor> &aiq_compositor)
    : _aiq_compositor (aiq_compositor)
    , _analyzer (analyzer)
    , _started (false)
{
    xcam_mem_clear (_ia_ae_window);
    xcam_mem_clear (_sensor_descriptor);
    xcam_mem_clear (_manual_limits);
    xcam_mem_clear (_input);
    mAeState = new RkAEStateMachine();
}

bool
AiqAeHandler::set_description (struct rkisp_sensor_mode_data *sensor_data)
{
    XCAM_ASSERT (sensor_data);

    _sensor_descriptor.pixel_clock_freq_mhz = sensor_data->vt_pix_clk_freq_mhz / 1000000.0f;
    _sensor_descriptor.pixel_periods_per_line = sensor_data->line_length_pck;
    _sensor_descriptor.line_periods_per_field = sensor_data->frame_length_lines;
    _sensor_descriptor.line_periods_vertical_blanking = sensor_data->frame_length_lines
            - (sensor_data->crop_vertical_end - sensor_data->crop_vertical_start + 1)
            / sensor_data->binning_factor_y;
    _sensor_descriptor.fine_integration_time_min = sensor_data->fine_integration_time_def;
    _sensor_descriptor.fine_integration_time_max_margin = sensor_data->line_length_pck - sensor_data->fine_integration_time_def;
    _sensor_descriptor.coarse_integration_time_min = sensor_data->coarse_integration_time_min;
    _sensor_descriptor.coarse_integration_time_max_margin = sensor_data->coarse_integration_time_max_margin;

    return true;
}

bool
AiqAeHandler::ensure_ia_parameters ()
{
    bool ret = true;
    return ret;
}

bool AiqAeHandler::ensure_ae_mode ()
{
    return true;
}
bool AiqAeHandler::ensure_ae_metering_mode ()
{
    return true;
}

bool AiqAeHandler::ensure_ae_priority_mode ()
{
    return true;
}

bool AiqAeHandler::ensure_ae_flicker_mode ()
{
    return true;
}

bool AiqAeHandler::ensure_ae_manual ()
{
    return true;
}

bool AiqAeHandler::ensure_ae_ev_shift ()
{
    return true;
}

SmartPtr<X3aResult>
AiqAeHandler::pop_result ()
{
    X3aIspExposureResult *result = new X3aIspExposureResult(XCAM_IMAGE_PROCESS_ONCE);
    struct rkisp_exposure sensor;
    XCam3aResultExposure exposure;

    xcam_mem_clear (sensor);
    sensor.coarse_integration_time = _result.regIntegrationTime;
    sensor.analog_gain = _result.regGain;
    sensor.digital_gain = 0;
    result->set_isp_config (sensor);

    xcam_mem_clear (exposure);
    exposure.exposure_time = _result.coarse_integration_time * 1000000;
    exposure.analog_gain = _result.analog_gain_code_global;
    exposure.digital_gain = 1.0f;
    exposure.aperture = _result.aperture_fn;
    result->set_standard_result (exposure);

#if 0
    XCAM_LOG_INFO ("AiqAeHandler, time-gain=[%d-%d]",
        _result.regIntegrationTime,
        _result.regGain);
#endif
    return result;
}

void
AiqAeHandler::convert_from_rkisp_aec_result(
        rk_aiq_ae_results* aec_result, AecResult_t* result) {
    struct CamIA10_SensorModeData &sensor_desc = _aiq_compositor->get_sensor_mode_data();
    aec_result->exposure.exposure_time_us = result->coarse_integration_time * 1000 * 1000;
    aec_result->exposure.analog_gain = result->analog_gain_code_global;

    //useless
    aec_result->exposure.digital_gain = result->analog_gain_code_global;
    aec_result->exposure.iso = result->analog_gain_code_global;

    aec_result->sensor_exposure.coarse_integration_time = result->regIntegrationTime;
    aec_result->sensor_exposure.analog_gain_code_global = result->regGain;

    //useless
    aec_result->sensor_exposure.fine_integration_time = result->regIntegrationTime;
    aec_result->sensor_exposure.digital_gain_global = result->gainFactor;

    aec_result->sensor_exposure.frame_length_lines = result->LinePeriodsPerField;
    aec_result->sensor_exposure.line_length_pixels = result->PixelPeriodsPerLine;

    aec_result->flicker_reduction_mode = rk_aiq_ae_flicker_reduction_50hz;

    // grid 5x5 maxsize=[2580x1950]
    aec_result->aec_config_result.enabled = true;
    aec_result->aec_config_result.mode = RK_ISP_EXP_MEASURING_MODE_0;
    aec_result->aec_config_result.win.width =
        result->meas_win.h_size > 2580 ? 2580 : result->meas_win.h_size; // 35 <= value <= 516
    aec_result->aec_config_result.win.height =
        result->meas_win.v_size > 1950 ? 1950 : result->meas_win.v_size; // 28 <= value <= 390
    aec_result->aec_config_result.win.h_offset = // 0 <= value <= 2424
        (sensor_desc.sensor_output_width - aec_result->aec_config_result.win.width) / 2;
    aec_result->aec_config_result.win.v_offset = // 0 <= value <= 1806
         (sensor_desc.sensor_output_height - aec_result->aec_config_result.win.height) / 2;

    aec_result->hist_config_result.enabled = true;
    aec_result->hist_config_result.mode = RK_ISP_HIST_MODE_RGB_COMBINED;
    aec_result->hist_config_result.stepSize = result->stepSize;
    aec_result->hist_config_result.weights_cnt = RK_AIQ_HISTOGRAM_WEIGHT_GRIDS_SIZE;
    memcpy(aec_result->hist_config_result.weights, result->GridWeights, sizeof(unsigned char)*RK_AIQ_HISTOGRAM_WEIGHT_GRIDS_SIZE);
    aec_result->hist_config_result.window.width = result->meas_win.h_size;
    aec_result->hist_config_result.window.height = result->meas_win.v_size;
    aec_result->hist_config_result.window.h_offset =
		(sensor_desc.sensor_output_width - aec_result->hist_config_result.window.width) / 2;
    aec_result->hist_config_result.window.v_offset =
		(sensor_desc.sensor_output_height - aec_result->hist_config_result.window.height) / 2;

    aec_result->converged = result->converged;

#if 0
    printf("aec converged: %d\n", aec_result->converged);

    printf("aec result: vts-hts=%d-%d \n", aec_result->sensor_exposure.frame_length_lines, aec_result->sensor_exposure.line_length_pixels);

    printf("interface check hist weights=[%d-%d-%d-%d-%d]\n",
		aec_result->hist_config_result.weights[0],
		aec_result->hist_config_result.weights[1],
		aec_result->hist_config_result.weights[2],
		aec_result->hist_config_result.weights[3],
		aec_result->hist_config_result.weights[12]);

    printf("interface check aec result win=[%d-%d-%d-%d]\n",
		aec_result->aec_config_result.win.h_offset,
		aec_result->aec_config_result.win.v_offset,
		aec_result->aec_config_result.win.width,
		aec_result->aec_config_result.win.height);

    printf("interface check hist result step=[%d] win=[%d-%d-%d-%d]\n",
		aec_result->hist_config_result.stepSize,
		aec_result->hist_config_result.window.h_offset,
		aec_result->hist_config_result.window.v_offset,
		aec_result->hist_config_result.window.width,
		aec_result->hist_config_result.window.height);
#endif

}

void
AiqAwbHandler::convert_from_rkisp_awb_result(
        rk_aiq_awb_results* aiq_awb_result, CamIA10_AWB_Result_t* result) {

    struct CamIA10_SensorModeData &sensor_desc = _aiq_compositor->get_sensor_mode_data();
    aiq_awb_result->awb_meas_cfg.enabled = true;
    aiq_awb_result->awb_meas_cfg.awb_meas_mode = RK_ISP_AWB_MEASURING_MODE_YCBCR;//result->MeasMode;
    aiq_awb_result->awb_meas_cfg.awb_meas_cfg.max_y= result->MeasConfig.MaxY;
    aiq_awb_result->awb_meas_cfg.awb_meas_cfg.ref_cr_max_r= result->MeasConfig.RefCr_MaxR;
    aiq_awb_result->awb_meas_cfg.awb_meas_cfg.min_y_max_g= result->MeasConfig.MinY_MaxG;
    aiq_awb_result->awb_meas_cfg.awb_meas_cfg.ref_cb_max_b= result->MeasConfig.RefCb_MaxB;
    aiq_awb_result->awb_meas_cfg.awb_meas_cfg.max_c_sum= result->MeasConfig.MaxCSum;
    aiq_awb_result->awb_meas_cfg.awb_meas_cfg.min_c= result->MeasConfig.MinC;

    aiq_awb_result->awb_meas_cfg.awb_win.h_offset = result->awbWin.h_offs;
    aiq_awb_result->awb_meas_cfg.awb_win.v_offset = result->awbWin.v_offs;
    aiq_awb_result->awb_meas_cfg.awb_win.width= result->awbWin.h_size;
    aiq_awb_result->awb_meas_cfg.awb_win.height = result->awbWin.v_size;

    //394-256-256-296
    aiq_awb_result->awb_gain_cfg.enabled = true;
    aiq_awb_result->awb_gain_cfg.awb_gains.red_gain = result->awbGains.Red == 0 ? 394 : result->awbGains.Red;
    aiq_awb_result->awb_gain_cfg.awb_gains.green_b_gain= result->awbGains.GreenB == 0 ? 256 : result->awbGains.GreenB;
    aiq_awb_result->awb_gain_cfg.awb_gains.green_r_gain= result->awbGains.GreenR == 0 ? 256 : result->awbGains.GreenR;
    aiq_awb_result->awb_gain_cfg.awb_gains.blue_gain= result->awbGains.Blue == 0 ? 296 : result->awbGains.Blue;

    //ALOGD("AWB GAIN RESULT: %d-%d-%d-%d", result->awbGains.Red, result->awbGains.GreenB, result->awbGains.GreenR, result->awbGains.Blue);

    aiq_awb_result->ctk_config.enabled = true;
    memcpy(aiq_awb_result->ctk_config.ctk_matrix.coeff, result->CcMatrix.Coeff, sizeof(unsigned int)*9);
    aiq_awb_result->ctk_config.cc_offset.red= result->CcOffset.Red;
    aiq_awb_result->ctk_config.cc_offset.green= result->CcOffset.Green;
    aiq_awb_result->ctk_config.cc_offset.blue= result->CcOffset.Blue;

    if (sensor_desc.sensor_output_width != 0 &&
		sensor_desc.sensor_output_height != 0) {
    aiq_awb_result->lsc_cfg.enabled = true;
    aiq_awb_result->lsc_cfg.config_width = sensor_desc.sensor_output_width;
    aiq_awb_result->lsc_cfg.config_height = sensor_desc.sensor_output_height;

    aiq_awb_result->lsc_cfg.lsc_config.lsc_size_tbl_cnt = RK_AIQ_LSC_SIZE_TBL_SIZE;
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_x_size_tbl,
        result->SectorConfig.LscXSizeTbl, RK_AIQ_LSC_SIZE_TBL_SIZE*sizeof(unsigned short));
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_y_size_tbl,
        result->SectorConfig.LscYSizeTbl, RK_AIQ_LSC_SIZE_TBL_SIZE*sizeof(unsigned short));

    aiq_awb_result->lsc_cfg.lsc_config.lsc_grad_tbl_cnt = RK_AIQ_LSC_GRAD_TBL_SIZE;
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_x_grad_tbl,
        result->SectorConfig.LscXGradTbl, RK_AIQ_LSC_GRAD_TBL_SIZE*sizeof(unsigned short));
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_y_grad_tbl,
        result->SectorConfig.LscYGradTbl, RK_AIQ_LSC_GRAD_TBL_SIZE*sizeof(unsigned short));

    aiq_awb_result->lsc_cfg.lsc_config.lsc_data_tbl_cnt = RK_AIQ_LSC_DATA_TBL_SIZE;
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_r_data_tbl,
        result->LscMatrixTable.LscMatrix[CAM_4CH_COLOR_COMPONENT_RED].uCoeff,
        RK_AIQ_LSC_DATA_TBL_SIZE*sizeof(unsigned short));
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_gr_data_tbl,
        result->LscMatrixTable.LscMatrix[CAM_4CH_COLOR_COMPONENT_GREENR].uCoeff,
        RK_AIQ_LSC_DATA_TBL_SIZE*sizeof(unsigned short));
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_gb_data_tbl,
        result->LscMatrixTable.LscMatrix[CAM_4CH_COLOR_COMPONENT_GREENB].uCoeff,
        RK_AIQ_LSC_DATA_TBL_SIZE*sizeof(unsigned short));
    memcpy(aiq_awb_result->lsc_cfg.lsc_config.lsc_b_data_tbl,
        result->LscMatrixTable.LscMatrix[CAM_4CH_COLOR_COMPONENT_BLUE].uCoeff,
        RK_AIQ_LSC_DATA_TBL_SIZE*sizeof(unsigned short));

    }

    aiq_awb_result->converged = result->converged;

    LOGI("awb converged: %d, R-B gain: %d-%d\n",
		aiq_awb_result->converged,
		aiq_awb_result->awb_gain_cfg.awb_gains.red_gain,
		aiq_awb_result->awb_gain_cfg.awb_gains.blue_gain);
#if 0

    printf("--awb config, max_y: %d, cr: %d, cb: %d, miny: %d, maxcsum: %d, minc: %d\n",
           aiq_awb_result->awb_meas_cfg.awb_meas_cfg.max_y,
           aiq_awb_result->awb_meas_cfg.awb_meas_cfg.ref_cr_max_r,
           aiq_awb_result->awb_meas_cfg.awb_meas_cfg.ref_cb_max_b,
           aiq_awb_result->awb_meas_cfg.awb_meas_cfg.min_y_max_g,
           aiq_awb_result->awb_meas_cfg.awb_meas_cfg.max_c_sum,
           aiq_awb_result->awb_meas_cfg.awb_meas_cfg.min_c);

    printf("interface check awb result win=[%d-%d-%d-%d]\n",
		aiq_awb_result->awb_meas_cfg.awb_win.h_offset,
		aiq_awb_result->awb_meas_cfg.awb_win.v_offset,
		aiq_awb_result->awb_meas_cfg.awb_win.width,
		aiq_awb_result->awb_meas_cfg.awb_win.height);

    printf("ctk offset=[%d-%d-%d]\n",
		aiq_awb_result->ctk_config.cc_offset.red,
		aiq_awb_result->ctk_config.cc_offset.green,
		aiq_awb_result->ctk_config.cc_offset.blue);
    printf("interface check awb ctk config=[%d-%d-%d]\n",
		aiq_awb_result->ctk_config.ctk_matrix.coeff[0],
		aiq_awb_result->ctk_config.ctk_matrix.coeff[1],
		aiq_awb_result->ctk_config.ctk_matrix.coeff[2]
		);
    printf("interface check awb ctk config=[%d-%d-%d]\n",
		aiq_awb_result->ctk_config.ctk_matrix.coeff[3],
		aiq_awb_result->ctk_config.ctk_matrix.coeff[4],
		aiq_awb_result->ctk_config.ctk_matrix.coeff[5]
		);
    printf("interface check awb ctk config=[%d-%d-%d]\n",
		aiq_awb_result->ctk_config.ctk_matrix.coeff[6],
		aiq_awb_result->ctk_config.ctk_matrix.coeff[7],
		aiq_awb_result->ctk_config.ctk_matrix.coeff[8]
		);
#endif
}

XCamReturn
AiqAeHandler::processAeMetaResults(AecResult_t aec_results, X3aResultList &output)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    SmartPtr<AiqInputParams> inputParams = _aiq_compositor->getAiqInputParams();
    SmartPtr<XmetaResult> res;
    for (X3aResultList::iterator iter = output.begin ();
            iter != output.end ();)
    {
        if ((*iter)->get_type() == XCAM_3A_METADATA_RESULT_TYPE) {
            res = (*iter).dynamic_cast_ptr<XmetaResult> ();
            break ;
        }
        ++iter;
        if (iter == output.end()) {
            res = new XmetaResult(XCAM_IMAGE_PROCESS_ONCE);
            output.push_back(res);
        }
    }

    CameraMetadata* metadata = res->get_metadata_result();

    XCamAeParam &aeParams = inputParams->aeInputParams.aeParams;
    uint8_t sceneFlickerMode = ANDROID_STATISTICS_SCENE_FLICKER_NONE;
    switch (aeParams.flicker_mode) {
    case XCAM_AE_FLICKER_MODE_50HZ:
        sceneFlickerMode = ANDROID_STATISTICS_SCENE_FLICKER_50HZ;
        break;
    case XCAM_AE_FLICKER_MODE_60HZ:
        sceneFlickerMode = ANDROID_STATISTICS_SCENE_FLICKER_60HZ;
        break;
    default:
        sceneFlickerMode = ANDROID_STATISTICS_SCENE_FLICKER_NONE;
    }
    //# ANDROID_METADATA_Dynamic android.statistics.sceneFlicker done
    metadata->update(ANDROID_STATISTICS_SCENE_FLICKER,
                                    &sceneFlickerMode, 1);

    convert_from_rkisp_aec_result(&_rkaiq_result, &aec_results);

    LOGD("%s exp_time=%d gain=%f", __FUNCTION__,
            _rkaiq_result.exposure.exposure_time_us,
            _rkaiq_result.exposure.analog_gain);

    ret = mAeState->processResult(_rkaiq_result, *metadata,
                            inputParams->reqId);

    /* not support aeRegions now */
    //# ANDROID_METADATA_Dynamic android.control.aeRegions done

    //# ANDROID_METADATA_Dynamic android.control.aeExposureCompensation done
    // TODO get step size (currently 1/3) from static metadata
    int32_t exposureCompensation =
            round((aeParams.ev_shift) * 3);

    metadata->update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
                                    &exposureCompensation,
                                    1);

    int64_t exposureTime = 0;
    uint16_t pixels_per_line = 0;
    uint16_t lines_per_frame = 0;
    int64_t manualExpTime = 1;
    struct CamIA10_SensorModeData &sensor_desc = _aiq_compositor->get_sensor_mode_data();

    if (inputParams->aaaControls.ae.aeMode != ANDROID_CONTROL_AE_MODE_OFF) {

        // Calculate frame duration from AE results and sensor descriptor
        pixels_per_line = _rkaiq_result.sensor_exposure.line_length_pixels;
        lines_per_frame = _rkaiq_result.sensor_exposure.frame_length_lines;

        /*
         * Android wants the frame duration in nanoseconds
         */
        int64_t frameDuration = (pixels_per_line * lines_per_frame) /
                                sensor_desc.pixel_clock_freq_mhz;
        frameDuration *= 1000;
        metadata->update(ANDROID_SENSOR_FRAME_DURATION,
                                             &frameDuration, 1);

        /*
         * AE reports exposure in usecs but Android wants it in nsecs
         * In manual mode, use input value if delta to expResult is small.
         */
        exposureTime = _rkaiq_result.exposure.exposure_time_us;
        manualExpTime = aeParams.manual_exposure_time;

        if (exposureTime == 0 ||
            (manualExpTime > 0 &&
            fabs((float)exposureTime/manualExpTime - 1) < 0.01)) {

            if (exposureTime == 0)
                LOGW("sensor exposure time is Zero, copy input value");
            // copy input value
            exposureTime = manualExpTime;
        }
        exposureTime = exposureTime * 1000; // to ns.
        metadata->update(ANDROID_SENSOR_EXPOSURE_TIME,
                                         &exposureTime, 1);
    }

    int32_t value = ANDROID_SENSOR_TEST_PATTERN_MODE_OFF;
    CameraMetadata *settings = &inputParams->settings;
    camera_metadata_entry entry = settings->find(ANDROID_SENSOR_TEST_PATTERN_MODE);
    if (entry.count == 1)
        value = entry.data.i32[0];

    metadata->update(ANDROID_SENSOR_TEST_PATTERN_MODE,
                                     &value, 1);

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
AiqAwbHandler::processAwbMetaResults(CamIA10_AWB_Result_t awb_results, X3aResultList &output)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    SmartPtr<AiqInputParams> inputParams = _aiq_compositor->getAiqInputParams();
    SmartPtr<XmetaResult> res;
    LOGI("@%s %d: enter", __FUNCTION__, __LINE__);

    for (X3aResultList::iterator iter = output.begin ();
            iter != output.end ();)
    {
        if ((*iter)->get_type() == XCAM_3A_METADATA_RESULT_TYPE) {
            res = (*iter).dynamic_cast_ptr<XmetaResult> ();
            break ;
        }
        ++iter;
        if (iter == output.end()) {
            res = new XmetaResult(XCAM_IMAGE_PROCESS_ONCE);
            output.push_back(res);
        }
    }

    CameraMetadata* metadata = res->get_metadata_result();
    convert_from_rkisp_awb_result(&_rkaiq_result, &awb_results);

    ret = mAwbState->processResult(_rkaiq_result, *metadata);

    metadata->update(ANDROID_COLOR_CORRECTION_MODE,
                  &inputParams->aaaControls.awb.colorCorrectionMode,
                  1);
    metadata->update(ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
                  &inputParams->aaaControls.awb.colorCorrectionAberrationMode,
                  1);
    /*
     * TODO: Consider moving this to common code in 3A class
     */
    float gains[4] = {1.0, 1.0, 1.0, 1.0};
    gains[0] = _rkaiq_result.awb_gain_cfg.awb_gains.red_gain;
    gains[1] = _rkaiq_result.awb_gain_cfg.awb_gains.green_r_gain;
    gains[2] = _rkaiq_result.awb_gain_cfg.awb_gains.green_b_gain;
    gains[3] = _rkaiq_result.awb_gain_cfg.awb_gains.blue_gain;
    metadata->update(ANDROID_COLOR_CORRECTION_GAINS, gains, 4);

    /*
     * store the results in row major order
     */
    camera_metadata_rational_t transformMatrix[9];
    const int32_t COLOR_TRANSFORM_PRECISION = 10000;
    for (int i = 0; i < 9; i++) {
        transformMatrix[i].numerator =
            (int32_t)(_rkaiq_result.ctk_config.ctk_matrix.coeff[i] * COLOR_TRANSFORM_PRECISION);
        transformMatrix[i].denominator = COLOR_TRANSFORM_PRECISION;

    }

    metadata->update(ANDROID_COLOR_CORRECTION_TRANSFORM,
                  transformMatrix, 9);
    return ret;
}

XCamReturn
AiqAeHandler::analyze (X3aResultList &output, bool first)
{
    XCAM_ASSERT (_analyzer);
    SmartPtr<AiqInputParams> inputParams = _aiq_compositor->getAiqInputParams();
    bool forceAeRun = false;

    if (inputParams.ptr()) {
        bool forceAeRun = _latestInputParams.aeInputParams.aeParams.ev_shift !=
            inputParams->aeInputParams.aeParams.ev_shift;

        // process state when the request is actually processed
        mAeState->processState(inputParams->aaaControls.controlMode,
                               inputParams->aaaControls.ae);

        _latestInputParams = *inputParams.ptr();
    }

    if (forceAeRun || mAeState->getState() != ANDROID_CONTROL_AE_STATE_LOCKED) {

        SmartPtr<X3aResult> result;
        if (inputParams.ptr())
            this->update_parameters (inputParams->aeInputParams.aeParams);
        XCamAeParam param = this->get_params_unlock ();

        _aiq_compositor->_isp10_engine->runAe(&param, &_result, first);
        result = pop_result ();
        mLastestAeresult = result;
        if (result.ptr())
            output.push_back (result);

    } else {
        if (mLastestAeresult.ptr())
            output.push_back (mLastestAeresult);
    }

    return XCAM_RETURN_NO_ERROR;
}

bool
AiqAeHandler::manual_control_result (
    ia_aiq_exposure_sensor_parameters &cur_res,
    ia_aiq_exposure_parameters &cur_aiq_exp,
    const ia_aiq_exposure_sensor_parameters &last_res)
{
    adjust_ae_speed (cur_res, cur_aiq_exp, last_res, this->get_speed_unlock());
    adjust_ae_limitation (cur_res, cur_aiq_exp);

    return true;
}

void
AiqAeHandler::adjust_ae_speed (
    ia_aiq_exposure_sensor_parameters &cur_res,
    ia_aiq_exposure_parameters &cur_aiq_exp,
    const ia_aiq_exposure_sensor_parameters &last_res,
    double ae_speed)
{
    double last_gain, input_gain, ret_gain;
    ia_aiq_exposure_sensor_parameters tmp_res;

    if (XCAM_DOUBLE_EQUAL_AROUND(ae_speed, 1.0 ))
        return;
    xcam_mem_clear (tmp_res);
    tmp_res.coarse_integration_time = _calculate_new_value_by_speed (
                                          last_res.coarse_integration_time,
                                          cur_res.coarse_integration_time,
                                          ae_speed);

    last_gain = _imx185_sensor_gain_code_to_mutiplier (last_res.analog_gain_code_global);
    input_gain = _imx185_sensor_gain_code_to_mutiplier (cur_res.analog_gain_code_global);
    ret_gain = _calculate_new_value_by_speed (last_gain, input_gain, ae_speed);

    tmp_res.analog_gain_code_global = _mutiplier_to_imx185_sensor_gain_code (ret_gain);

    XCAM_LOG_DEBUG ("AE speed: from (shutter:%d, gain:%d[%.03f]) to (shutter:%d, gain:%d[%.03f])",
                    cur_res.coarse_integration_time, cur_res.analog_gain_code_global, input_gain,
                    tmp_res.coarse_integration_time, tmp_res.analog_gain_code_global, ret_gain);

    cur_res.coarse_integration_time = tmp_res.coarse_integration_time;
    cur_res.analog_gain_code_global = tmp_res.analog_gain_code_global;
    cur_aiq_exp.exposure_time_us = _coarse_line_to_time (&_sensor_descriptor,
                                   cur_res.coarse_integration_time);
    cur_aiq_exp.analog_gain = ret_gain;
}

void
AiqAeHandler::adjust_ae_limitation (ia_aiq_exposure_sensor_parameters &cur_res,
                                    ia_aiq_exposure_parameters &cur_aiq_exp)
{
    ia_aiq_exposure_sensor_descriptor * desc = &_sensor_descriptor;
    uint64_t exposure_min = 0, exposure_max = 0;
    double analog_max = get_max_analog_gain_unlock ();
    uint32_t min_coarse_value = desc->coarse_integration_time_min;
    uint32_t max_coarse_value = desc->line_periods_per_field - desc->coarse_integration_time_max_margin;
    uint32_t value;

    get_exposure_time_range_unlock (exposure_min, exposure_max);

    if (exposure_min) {
        value = _time_to_coarse_line (desc, (uint32_t)exposure_min);
        min_coarse_value = (value > min_coarse_value) ? value : min_coarse_value;
    }
    if (cur_res.coarse_integration_time < min_coarse_value) {
        cur_res.coarse_integration_time = min_coarse_value;
        cur_aiq_exp.exposure_time_us = _coarse_line_to_time (desc, min_coarse_value);
    }

    if (exposure_max) {
        value = _time_to_coarse_line (desc, (uint32_t)exposure_max);
        max_coarse_value = (value < max_coarse_value) ? value : max_coarse_value;
    }
    if (cur_res.coarse_integration_time > max_coarse_value) {
        cur_res.coarse_integration_time = max_coarse_value;
        cur_aiq_exp.exposure_time_us = _coarse_line_to_time (desc, max_coarse_value);
    }

    if (analog_max >= 1.0) {
        /* limit gains */
        double gain = _imx185_sensor_gain_code_to_mutiplier (cur_res.analog_gain_code_global);
        if (gain > analog_max) {
            cur_res.analog_gain_code_global = _mutiplier_to_imx185_sensor_gain_code (analog_max);
            cur_aiq_exp.analog_gain = analog_max;
        }
    }
}

XCamFlickerMode
AiqAeHandler::get_flicker_mode ()
{
    {
        AnalyzerHandler::HandlerLock lock(this);
    }
    return AeHandler::get_flicker_mode ();
}

int64_t
AiqAeHandler::get_current_exposure_time ()
{
    AnalyzerHandler::HandlerLock lock(this);

    return (int64_t)_result.coarse_integration_time;
}

double
AiqAeHandler::get_current_analog_gain ()
{
    AnalyzerHandler::HandlerLock lock(this);
    return (double)_result.analog_gain_code_global;
}

double
AiqAeHandler::get_max_analog_gain ()
{
    {
        AnalyzerHandler::HandlerLock lock(this);
    }
    return AeHandler::get_max_analog_gain ();
}

XCamReturn
AiqAeHandler::set_RGBS_weight_grid (ia_aiq_rgbs_grid **out_rgbs_grid)
{
    AnalyzerHandler::HandlerLock lock(this);

    rgbs_grid_block *rgbs_grid_ptr = (*out_rgbs_grid)->blocks_ptr;
    uint32_t rgbs_grid_index = 0;
    uint16_t rgbs_grid_width = (*out_rgbs_grid)->grid_width;
    uint16_t rgbs_grid_height = (*out_rgbs_grid)->grid_height;

    XCAM_LOG_DEBUG ("rgbs_grid_width = %d, rgbs_grid_height = %d", rgbs_grid_width, rgbs_grid_height);

    uint64_t weight_sum = 0;

    uint32_t image_width = 0;
    uint32_t image_height = 0;
    _aiq_compositor->get_size (image_width, image_height);
    XCAM_LOG_DEBUG ("image_width = %d, image_height = %d", image_width, image_height);

    uint32_t hor_pixels_per_grid = (image_width + (rgbs_grid_width >> 1)) / rgbs_grid_width;
    uint32_t vert_pixels_per_gird = (image_height + (rgbs_grid_height >> 1)) / rgbs_grid_height;
    XCAM_LOG_DEBUG ("rgbs grid: %d x %d pixels per grid cell", hor_pixels_per_grid, vert_pixels_per_gird);

    XCam3AWindow weighted_window = this->get_window_unlock ();
    uint32_t weighted_grid_width = ((weighted_window.x_end - weighted_window.x_start + 1) +
                                    (hor_pixels_per_grid >> 1)) / hor_pixels_per_grid;
    uint32_t weighted_grid_height = ((weighted_window.y_end - weighted_window.y_start + 1) +
                                     (vert_pixels_per_gird >> 1)) / vert_pixels_per_gird;
    XCAM_LOG_DEBUG ("weighted_grid_width = %d, weighted_grid_height = %d", weighted_grid_width, weighted_grid_height);

    uint32_t *weighted_avg_gr = (uint32_t*)xcam_malloc0 (5 * weighted_grid_width * weighted_grid_height * sizeof(uint32_t));
    if (NULL == weighted_avg_gr) {
        return XCAM_RETURN_ERROR_MEM;
    }
    uint32_t *weighted_avg_r = weighted_avg_gr + (weighted_grid_width * weighted_grid_height);
    uint32_t *weighted_avg_b = weighted_avg_r + (weighted_grid_width * weighted_grid_height);
    uint32_t *weighted_avg_gb = weighted_avg_b + (weighted_grid_width * weighted_grid_height);
    uint32_t *weighted_sat = weighted_avg_gb + (weighted_grid_width * weighted_grid_height);

    for (uint32_t win_index = 0; win_index < XCAM_AE_MAX_METERING_WINDOW_COUNT; win_index++) {
        XCAM_LOG_DEBUG ("window start point(%d, %d), end point(%d, %d), weight = %d",
                        _params.window_list[win_index].x_start, _params.window_list[win_index].y_start,
                        _params.window_list[win_index].x_end, _params.window_list[win_index].y_end,
                        _params.window_list[win_index].weight);

        if ((_params.window_list[win_index].weight <= 0) ||
                (_params.window_list[win_index].x_start < 0) ||
                ((uint32_t)_params.window_list[win_index].x_end > image_width) ||
                (_params.window_list[win_index].y_start < 0) ||
                ((uint32_t)_params.window_list[win_index].y_end > image_height) ||
                (_params.window_list[win_index].x_start >= _params.window_list[win_index].x_end) ||
                (_params.window_list[win_index].y_start >= _params.window_list[win_index].y_end) ||
                ((uint32_t)_params.window_list[win_index].x_end - (uint32_t)_params.window_list[win_index].x_start > image_width) ||
                ((uint32_t)_params.window_list[win_index].y_end - (uint32_t)_params.window_list[win_index].y_start > image_height)) {
            XCAM_LOG_DEBUG ("skip window index = %d ", win_index);
            continue;
        }

        rgbs_grid_index = (_params.window_list[win_index].x_start +
                           (hor_pixels_per_grid >> 1)) / hor_pixels_per_grid +
                          ((_params.window_list[win_index].y_start + (vert_pixels_per_gird >> 1))
                           / vert_pixels_per_gird) * rgbs_grid_width;

        weight_sum += _params.window_list[win_index].weight;

        XCAM_LOG_DEBUG ("cumulate rgbs grid statistic, window index = %d ", win_index);
        for (uint32_t i = 0; i < weighted_grid_height; i++) {
            for (uint32_t j = 0; j < weighted_grid_width; j++) {
                weighted_avg_gr[j + i * weighted_grid_width] += rgbs_grid_ptr[rgbs_grid_index + j +
                        i * rgbs_grid_width].avg_gr * _params.window_list[win_index].weight;
                weighted_avg_r[j + i * weighted_grid_width] += rgbs_grid_ptr[rgbs_grid_index + j +
                        i * rgbs_grid_width].avg_r * _params.window_list[win_index].weight;
                weighted_avg_b[j + i * weighted_grid_width] += rgbs_grid_ptr[rgbs_grid_index + j +
                        i * rgbs_grid_width].avg_b * _params.window_list[win_index].weight;
                weighted_avg_gb[j + i * weighted_grid_width] += rgbs_grid_ptr[rgbs_grid_index + j +
                        i * rgbs_grid_width].avg_gb * _params.window_list[win_index].weight;
                weighted_sat[j + i * weighted_grid_width] += rgbs_grid_ptr[rgbs_grid_index + j +
                        i * rgbs_grid_width].sat * _params.window_list[win_index].weight;
            }
        }
    }
    XCAM_LOG_DEBUG ("sum of weighing factor = %" PRIu64, weight_sum);

    rgbs_grid_index = (weighted_window.x_start + (hor_pixels_per_grid >> 1)) / hor_pixels_per_grid +
                      (weighted_window.y_start + (vert_pixels_per_gird >> 1)) / vert_pixels_per_gird * rgbs_grid_width;
    for (uint32_t i = 0; i < weighted_grid_height; i++) {
        for (uint32_t j = 0; j < weighted_grid_width; j++) {
            rgbs_grid_ptr[rgbs_grid_index + j + i * rgbs_grid_width].avg_gr =
                weighted_avg_gr[j + i * weighted_grid_width] / weight_sum;
            rgbs_grid_ptr[rgbs_grid_index + j + i * rgbs_grid_width].avg_r =
                weighted_avg_r[j + i * weighted_grid_width] / weight_sum;
            rgbs_grid_ptr[rgbs_grid_index + j + i * rgbs_grid_width].avg_b =
                weighted_avg_b[j + i * weighted_grid_width] / weight_sum;
            rgbs_grid_ptr[rgbs_grid_index + j + i * rgbs_grid_width].avg_gb =
                weighted_avg_gb[j + i * weighted_grid_width] / weight_sum;
            rgbs_grid_ptr[rgbs_grid_index + j + i * rgbs_grid_width].sat =
                weighted_sat[j + i * weighted_grid_width] / weight_sum;
        }
    }

    xcam_free (weighted_avg_gr);

    return XCAM_RETURN_NO_ERROR;
}


XCamReturn
AiqAeHandler::set_hist_weight_grid (ia_aiq_hist_weight_grid **out_weight_grid)
{
    AnalyzerHandler::HandlerLock lock(this);

    uint16_t hist_grid_width = (*out_weight_grid)->width;
    uint16_t hist_grid_height = (*out_weight_grid)->height;
    uint32_t hist_grid_index = 0;

    unsigned char* weights_map_ptr = (*out_weight_grid)->weights;

    uint32_t image_width = 0;
    uint32_t image_height = 0;
    _aiq_compositor->get_size (image_width, image_height);

    uint32_t hor_pixels_per_grid = (image_width + (hist_grid_width >> 1)) / hist_grid_width;
    uint32_t vert_pixels_per_gird = (image_height + (hist_grid_height >> 1)) / hist_grid_height;
    XCAM_LOG_DEBUG ("hist weight grid: %d x %d pixels per grid cell", hor_pixels_per_grid, vert_pixels_per_gird);

    memset (weights_map_ptr, 0, hist_grid_width * hist_grid_height);

    for (uint32_t win_index = 0; win_index < XCAM_AE_MAX_METERING_WINDOW_COUNT; win_index++) {
        XCAM_LOG_DEBUG ("window start point(%d, %d), end point(%d, %d), weight = %d",
                        _params.window_list[win_index].x_start, _params.window_list[win_index].y_start,
                        _params.window_list[win_index].x_end, _params.window_list[win_index].y_end,
                        _params.window_list[win_index].weight);

        if ((_params.window_list[win_index].weight <= 0) ||
                (_params.window_list[win_index].weight > 15) ||
                (_params.window_list[win_index].x_start < 0) ||
                ((uint32_t)_params.window_list[win_index].x_end > image_width) ||
                (_params.window_list[win_index].y_start < 0) ||
                ((uint32_t)_params.window_list[win_index].y_end > image_height) ||
                (_params.window_list[win_index].x_start >= _params.window_list[win_index].x_end) ||
                (_params.window_list[win_index].y_start >= _params.window_list[win_index].y_end) ||
                ((uint32_t)_params.window_list[win_index].x_end - (uint32_t)_params.window_list[win_index].x_start > image_width) ||
                ((uint32_t)_params.window_list[win_index].y_end - (uint32_t)_params.window_list[win_index].y_start > image_height)) {
            XCAM_LOG_DEBUG ("skip window index = %d ", win_index);
            continue;
        }

        uint32_t weighted_grid_width =
            ((_params.window_list[win_index].x_end - _params.window_list[win_index].x_start + 1) +
             (hor_pixels_per_grid >> 1)) / hor_pixels_per_grid;
        uint32_t weighted_grid_height =
            ((_params.window_list[win_index].y_end - _params.window_list[win_index].y_start + 1) +
             (vert_pixels_per_gird >> 1)) / vert_pixels_per_gird;

        hist_grid_index = (_params.window_list[win_index].x_start + (hor_pixels_per_grid >> 1)) / hor_pixels_per_grid +
                          ((_params.window_list[win_index].y_start + (vert_pixels_per_gird >> 1)) /
                           vert_pixels_per_gird) * hist_grid_width;

        for (uint32_t i = 0; i < weighted_grid_height; i++) {
            for (uint32_t j = 0; j < weighted_grid_width; j++) {
                weights_map_ptr[hist_grid_index + j + i * hist_grid_width] = _params.window_list[win_index].weight;
            }
        }
    }
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
AiqAeHandler::dump_hist_weight_grid (const ia_aiq_hist_weight_grid *weight_grid)
{
    XCAM_LOG_DEBUG ("E dump_hist_weight_grid");
    if (NULL == weight_grid) {
        return XCAM_RETURN_ERROR_PARAM;
    }

    uint16_t grid_width = weight_grid->width;
    uint16_t grid_height = weight_grid->height;

    for (uint32_t i = 0; i < grid_height; i++) {
        for (uint32_t j = 0; j < grid_width; j++) {
            printf ("%d  ", weight_grid->weights[j + i * grid_width]);
        }
        printf("\n");
    }

    XCAM_LOG_DEBUG ("X dump_hist_weight_grid");
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
AiqAeHandler::dump_RGBS_grid (const ia_aiq_rgbs_grid *rgbs_grid)
{
    XCAM_LOG_DEBUG ("E dump_RGBS_grid");
    if (NULL == rgbs_grid) {
        return XCAM_RETURN_ERROR_PARAM;
    }

    uint16_t grid_width = rgbs_grid->grid_width;
    uint16_t grid_height = rgbs_grid->grid_height;

    printf("AVG B\n");
    for (uint32_t i = 0; i < grid_height; i++) {
        for (uint32_t j = 0; j < grid_width; j++) {
            printf ("%d  ", rgbs_grid->blocks_ptr[j + i * grid_width].avg_b);
        }
        printf("\n");
    }
    printf("AVG Gb\n");
    for (uint32_t i = 0; i < grid_height; i++) {
        for (uint32_t j = 0; j < grid_width; j++) {
            printf ("%d  ", rgbs_grid->blocks_ptr[j + i * grid_width].avg_gb);
        }
        printf("\n");
    }
    printf("AVG Gr\n");
    for (uint32_t i = 0; i < grid_height; i++) {
        for (uint32_t j = 0; j < grid_width; j++) {
            printf ("%d  ", rgbs_grid->blocks_ptr[j + i * grid_width].avg_gr);
        }
        printf("\n");
    }
    printf("AVG R\n");
    for (uint32_t i = 0; i < grid_height; i++) {
        for (uint32_t j = 0; j < grid_width; j++) {
            printf ("%d  ", rgbs_grid->blocks_ptr[j + i * grid_width].avg_r);
            //printf ("%d  ", rgbs_grid->blocks_ptr[j + i * grid_width].sat);
        }
        printf("\n");
    }

    XCAM_LOG_DEBUG ("X dump_RGBS_grid");
    return XCAM_RETURN_NO_ERROR;
}

AiqAwbHandler::AiqAwbHandler (X3aAnalyzerRKiq *analyzer, SmartPtr<RKiqCompositor> &aiq_compositor)
    : _aiq_compositor (aiq_compositor)
    , _analyzer (analyzer)
    , _started (false)
{
    xcam_mem_clear (_cct_range);
    xcam_mem_clear (_result);
    xcam_mem_clear (_history_result);
    xcam_mem_clear (_cct_range);
    xcam_mem_clear (_input);
    mAwbState = new RkAWBStateMachine();
}

XCamReturn
AiqAwbHandler::analyze (X3aResultList &output, bool first)
{

    XCAM_ASSERT (_analyzer);
    SmartPtr<AiqInputParams> inputParams = _aiq_compositor->getAiqInputParams();
    bool forceAeRun = false;

    if (inputParams.ptr()) {
        bool forceAwbRun = (inputParams->reqId == 0);

        // process state when the request is actually processed
        mAwbState->processState(inputParams->aaaControls.controlMode,
                               inputParams->aaaControls.awb);
    }

    if (forceAeRun || mAwbState->getState() != ANDROID_CONTROL_AWB_STATE_LOCKED) {

        if (inputParams.ptr())
            this->update_parameters (inputParams->awbInputParams.awbParams);

        //ensure_ia_parameters();
        XCamAwbParam param = this->get_params_unlock ();
        _aiq_compositor->_isp10_engine->runAwb(&param, &_result);
    }

    return XCAM_RETURN_NO_ERROR;
}

bool
AiqAwbHandler::ensure_ia_parameters ()
{
    bool ret = true;

    ret = ret && ensure_awb_mode ();
    return ret;
}

bool
AiqAwbHandler::ensure_awb_mode ()
{
    return true;
}

void
AiqAwbHandler::adjust_speed (const ia_aiq_awb_results &last_ret)
{
/* TODO
    _result.final_r_per_g =
        _calculate_new_value_by_speed (
            last_ret.final_r_per_g, _result.final_r_per_g, get_speed_unlock ());
    _result.final_b_per_g =
        _calculate_new_value_by_speed (
            last_ret.final_b_per_g, _result.final_b_per_g, get_speed_unlock ());
*/
}

uint32_t
AiqAwbHandler::get_current_estimate_cct ()
{
    AnalyzerHandler::HandlerLock lock(this);
    // TODO
    return 0;//(uint32_t)_result.cct_estimate;
}

XCamReturn
AiqAfHandler::analyze (X3aResultList &output, bool first)
{
    // TODO
    XCAM_UNUSED (output);
    XCam3aResultFocus isp_result;
    xcam_mem_clear(isp_result);
    XCamAfParam param = this->get_params_unlock();
    _aiq_compositor->_isp10_engine->runAf(&param, &isp_result);

    XCAM_LOG_INFO ("AiqAfHandler, position: %d",
        isp_result.next_lens_position);

    X3aIspFocusResult *result = new X3aIspFocusResult(XCAM_IMAGE_PROCESS_ONCE);
    struct rkisp_focus focus;
    focus.next_lens_position = isp_result.next_lens_position;
    result->set_isp_config (focus);
    result->set_standard_result (isp_result);
    output.push_back (result);
    return XCAM_RETURN_NO_ERROR;
}

AiqCommonHandler::AiqCommonHandler (SmartPtr<RKiqCompositor> &aiq_compositor)
    : _aiq_compositor (aiq_compositor)
    , _gbce_result (NULL)
{
}

XCamReturn
AiqCommonHandler::analyze (X3aResultList &output, bool first)
{
    //XCAM_LOG_INFO ("---------------run analyze");

    return XCAM_RETURN_NO_ERROR;
}

void
RKiqCompositor::convert_window_to_ia (const XCam3AWindow &window, ia_rectangle &ia_window)
{
    ia_rectangle source;
    ia_coordinate_system source_system;
    ia_coordinate_system target_system = {IA_COORDINATE_TOP, IA_COORDINATE_LEFT, IA_COORDINATE_BOTTOM, IA_COORDINATE_RIGHT};

    source_system.left = 0;
    source_system.top = 0;
    source_system.right = this->_width;
    source_system.bottom = this->_height;
    XCAM_ASSERT (_width && _height);

    source.left = window.x_start;
    source.top = window.y_start;
    source.right = window.x_end;
    source.bottom = window.y_end;
    //ia_coordinate_convert_rect (&source_system, &source, &target_system, &ia_window);
}

RKiqCompositor::RKiqCompositor ()
    : _inputParams(NULL)
    , _ia_handle (NULL)
    , _ia_mkn (NULL)
    , _pa_result (NULL)
    , _frame_use (ia_aiq_frame_use_video)
    , _width (0)
    , _height (0)
    , _isp10_engine(NULL)
{
    xcam_mem_clear (_frame_params);
    xcam_mem_clear (_isp_stats);
    xcam_mem_clear (_ia_stat);
    xcam_mem_clear (_ia_dcfg);
    xcam_mem_clear (_ia_results);
    xcam_mem_clear (_isp_cfg);

    _handle_manager = new X3aHandlerManager();
#if 1
    _ae_desc = _handle_manager->get_ae_handler_desc();
    _awb_desc = _handle_manager->get_awb_handler_desc();
    _af_desc = _handle_manager->get_af_handler_desc();

#else
    _ae_desc = X3aHandlerManager::instance()->get_ae_handler_desc();
    _awb_desc = X3aHandlerManager::instance()->get_awb_handler_desc();
    _af_desc = X3aHandlerManager::instance()->get_af_handler_desc();
#endif
    XCAM_LOG_DEBUG ("RKiqCompositor constructed");
}

RKiqCompositor::~RKiqCompositor ()
{
    if (!_isp10_engine) {
        delete _isp10_engine;
        _isp10_engine = NULL;
    }

    XCAM_LOG_DEBUG ("~RKiqCompositor destructed");
}

bool
RKiqCompositor::open (ia_binary_data &cpf)
{
    XCAM_LOG_DEBUG ("Aiq compositor opened");
    return true;
}

void
RKiqCompositor::close ()
{
    XCAM_LOG_DEBUG ("Aiq compositor closed");
}

void RKiqCompositor::set_isp_ctrl_device(Isp10Engine* dev) {
    if (dev == NULL) {
        XCAM_LOG_ERROR ("ISP control device is null");
        return;
    }

    _isp10_engine = dev;
    _isp10_engine->setExternalAEHandlerDesc(_ae_desc);
    _isp10_engine->setExternalAWBHandlerDesc(_awb_desc);
    _isp10_engine->setExternalAFHandlerDesc(_af_desc);
}

bool
RKiqCompositor::set_sensor_mode_data (struct isp_supplemental_sensor_mode_data *sensor_mode)
{
    if (!_isp10_engine) {
        XCAM_LOG_ERROR ("ISP control device is null");
        return false;
    }

    _ia_dcfg = *(_isp10_engine->getDynamicISPConfig());
    _isp10_engine->getSensorModedata(sensor_mode,  &_ia_dcfg.sensor_mode);
    _isp10_engine->updateDynamicConfig(&_ia_dcfg);
    _ia_stat.sensor_mode = _ia_dcfg.sensor_mode;

    return true;
}

bool
RKiqCompositor::set_3a_stats (SmartPtr<X3aIspStatistics> &stats)
{
    if (!_isp10_engine) {
        XCAM_LOG_ERROR ("ISP control device is null");
        return false;
    }

    _isp_stats = *(struct cifisp_stat_buffer*)stats->get_isp_stats();
    XCAM_LOG_DEBUG ("set_3a_stats meas type: %d", _isp_stats.meas_type);

    _isp_stats.meas_type = CIFISP_STAT_AUTOEXP | CIFISP_STAT_HIST | CIFISP_STAT_AWB | CIFISP_STAT_AFM_FIN;
    _isp10_engine->convertIspStats(&_isp_stats, &_ia_stat);
    _isp10_engine->setStatistics(&_ia_stat);
    return true;
}

XCamReturn RKiqCompositor::convert_color_effect (IspInputParameters &isp_input)
{
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
RKiqCompositor::apply_gamma_table (struct rkisp_parameters *isp_param)
{
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
RKiqCompositor::apply_night_mode (struct rkisp_parameters *isp_param)
{
    return XCAM_RETURN_NO_ERROR;
}

double
RKiqCompositor::calculate_value_by_factor (double factor, double min, double mid, double max)
{
    XCAM_ASSERT (factor >= -1.0 && factor <= 1.0);
    XCAM_ASSERT (min <= mid && max >= mid);

    if (factor >= 0.0)
        return (mid * (1.0 - factor) + max * factor);
    else
        return (mid * (1.0 + factor) + min * (-factor));
}

XCamReturn
RKiqCompositor::limit_nr_levels (struct rkisp_parameters *isp_param)
{
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn RKiqCompositor::integrate (X3aResultList &results)
{
    SmartPtr<X3aResult> isp_results;
    struct rkisp_parameters isp_3a_result;

    if (!_isp10_engine)
        XCAM_LOG_ERROR ("ISP control device is null");

    xcam_mem_clear(isp_3a_result);

    //_isp10_engine->runIA(&_ia_dcfg, &_ia_stat, &_ia_results);
    _isp10_engine->getIAResult(&_ia_results);

    if (!_isp10_engine->runISPManual(&_ia_results, BOOL_TRUE)) {
        XCAM_LOG_ERROR("%s:run ISP manual failed!", __func__);
    }

    if (_ae_handler.ptr() && _awb_handler.ptr() && _inputParams.ptr()) {
        _ae_handler->processAeMetaResults(_ia_results.aec, results);
        _awb_handler->processAwbMetaResults(_ia_results.awb, results);
    }

    _isp10_engine->convertIAResults(&_isp_cfg, &_ia_results);

    isp_3a_result.active_configs = _isp_cfg.active_configs;
    isp_3a_result.dpcc_config = _isp_cfg.configs.dpcc_config;
    isp_3a_result.bls_config = _isp_cfg.configs.bls_config;
    isp_3a_result.sdg_config = _isp_cfg.configs.sdg_config;
    isp_3a_result.hst_config = _isp_cfg.configs.hst_config;
    isp_3a_result.lsc_config = _isp_cfg.configs.lsc_config;
    isp_3a_result.awb_gain_config = _isp_cfg.configs.awb_gain_config;
    isp_3a_result.awb_meas_config = _isp_cfg.configs.awb_meas_config;
    isp_3a_result.flt_config = _isp_cfg.configs.flt_config;
    isp_3a_result.bdm_config = _isp_cfg.configs.bdm_config;
    isp_3a_result.ctk_config = _isp_cfg.configs.ctk_config;
    isp_3a_result.goc_config = _isp_cfg.configs.goc_config;
    isp_3a_result.cproc_config = _isp_cfg.configs.cproc_config;
    isp_3a_result.aec_config = _isp_cfg.configs.aec_config;
    isp_3a_result.afc_config = _isp_cfg.configs.afc_config;
    isp_3a_result.ie_config = _isp_cfg.configs.ie_config;
    isp_3a_result.dpf_config = _isp_cfg.configs.dpf_config;
    isp_3a_result.dpf_strength_config = _isp_cfg.configs.dpf_strength_config;
    isp_3a_result.aec_config = _isp_cfg.configs.aec_config;
    isp_3a_result.flt_denoise_level= _isp_cfg.configs.flt_denoise_level;
    isp_3a_result.flt_sharp_level= _isp_cfg.configs.flt_sharp_level;

    for (int i=0; i < HAL_ISP_MODULE_MAX_ID_ID + 1; i++) {
        isp_3a_result.enabled[i] = _isp_cfg.enabled[i];
    }

    isp_results = generate_3a_configs (&isp_3a_result);
    results.push_back (isp_results);

    _isp10_engine->applyIspConfig(&_isp_cfg);

    return XCAM_RETURN_NO_ERROR;
}

SmartPtr<X3aResult>
RKiqCompositor::generate_3a_configs (struct rkisp_parameters *parameters)
{
    SmartPtr<X3aResult> ret;

    X3aAtomIspParametersResult *x3a_result =
        new X3aAtomIspParametersResult (XCAM_IMAGE_PROCESS_ONCE);
    x3a_result->set_isp_config (*parameters);
    ret = x3a_result;

    return ret;
}

void
RKiqCompositor::set_ae_handler (SmartPtr<AiqAeHandler> &handler)
{
    XCAM_ASSERT (!_ae_handler.ptr());
    _ae_handler = handler;
}

void
RKiqCompositor::set_awb_handler (SmartPtr<AiqAwbHandler> &handler)
{
    XCAM_ASSERT (!_awb_handler.ptr());
    _awb_handler = handler;
}

void
RKiqCompositor::set_af_handler (SmartPtr<AiqAfHandler> &handler)
{
    XCAM_ASSERT (!_af_handler.ptr());
    _af_handler = handler;
}

void
RKiqCompositor::set_common_handler (SmartPtr<AiqCommonHandler> &handler)
{
    XCAM_ASSERT (!_common_handler.ptr());
    _common_handler = handler;
}


};