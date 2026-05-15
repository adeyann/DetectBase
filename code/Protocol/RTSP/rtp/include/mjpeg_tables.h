/***************************************************************************************
 *
 *  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
 *
 *  By downloading, copying, installing or using the software you agree to this license.
 *  If you do not agree to this license, do not download, install, 
 *  copy or use the software.
 *
 *  Copyright (C) 2014-2019, Happytimesoft Corporation, all rights reserved.
 *
 *  Redistribution and use in binary forms, with or without modification, are permitted.
 *
 *  Unless required by applicable law or agreed to in writing, software distributed 
 *  under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 *  CONDITIONS OF ANY KIND, either express or implied. See the License for the specific
 *  language governing permissions and limitations under the License.
 *
****************************************************************************************/

#ifndef MJPEG_TABLES_H
#define MJPEG_TABLES_H

#ifdef __cplusplus
extern "C" {
#endif

extern uint8 const lum_dc_codelens[16];
extern uint8 const lum_dc_symbols[12];
extern uint8 const lum_ac_codelens[16];
extern uint8 const lum_ac_symbols[162];
extern uint8 const chm_dc_codelens[16];
extern uint8 const chm_dc_symbols[12];
extern uint8 const chm_ac_codelens[16];
extern uint8 const chm_ac_symbols[162];

#ifdef __cplusplus
}
#endif

#endif // MJPEG_TABLES_H


