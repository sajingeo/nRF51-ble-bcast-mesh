/***********************************************************************************
Copyright (c) Nordic Semiconductor ASA
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of other
  contributors to this software may be used to endorse or promote products
  derived from this software without specific prior written permission.

  4. This software must only be used in a processor manufactured by Nordic
  Semiconductor ASA, or in a processor manufactured by a third party that
  is used in combination with a processor manufactured by Nordic Semiconductor.


THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************/
#include "mesh_gatt.h"

#include "rbc_mesh.h"
#include "version_handler.h"

#include "ble_err.h"

typedef struct
{
    uint16_t service_handle;
    ble_gatts_char_handles_t ble_md_char_handles;
    ble_gatts_char_handles_t ble_val_char_handles;
} mesh_srv_t;
/*****************************************************************************
* Static globals
*****************************************************************************/
static mesh_srv_t m_mesh_service = {0, 0, {0}};

static const ble_uuid128_t m_mesh_base_uuid = {{0x1E, 0xCD, 0x00, 0x00,
                                            0x8C, 0xB9, 0xA8, 0x8B,
                                            0x82, 0xD8, 0x51, 0xFD,
                                            0xA1, 0x77, 0x1E, 0x2A}};
static uint8_t m_mesh_base_uuid_type;

static bool m_is_initialized = false;

static uint16_t m_active_conn_handle = CONN_HANDLE_INVALID;

typedef enum
{
    MESH_GATT_EVT_OPCODE_DATA,
    MESH_GATT_EVT_OPCODE_FLAG_SET,
    MESH_GATT_EVT_OPCODE_FLAG_REQ,
    MESH_GATT_EVT_OPCODE_FLAG_RSP,
    MESH_GATT_EVT_OPCODE_ERROR_BUSY,
    MESH_GATT_EVT_OPCODE_ERROR_NOT_FOUND,
    MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE,
    MESH_GATT_EVT_OPCODE_ERROR_UNKNOWN_FLAG,
    MESH_GATT_EVT_OPCODE_ERROR_INVALID_OPCODE,
} mesh_gatt_evt_opcode_t;

typedef enum
{
    MESH_GATT_EVT_FLAG_PERSISTENT,
    MESH_GATT_EVT_FLAG_DO_TX
} mesh_gatt_evt_flag_t;

typedef struct
{
    uint8_t opcode;
    union {
        struct {
            rbc_mesh_value_handle_t handle;
            uint8_t data_len;
            uint8_t data[RBC_MESH_VALUE_MAX_LEN];
        } data_update;
        struct {
            rbc_mesh_value_handle_t handle;
            uint8_t flag;
            uint8_t value;
        } flag_update;
    } __packed_gcc __packed_armcc param;
} __packed_gcc mesh_gatt_evt_t;

/*****************************************************************************
* Static functions
*****************************************************************************/
static uint32_t mesh_gatt_evt_push(mesh_gatt_evt_t* p_gatt_evt)
{
    if (m_active_conn_handle != CONN_HANDLE_INVALID)
    {
        return BLE_ERROR_INVALID_CONN_HANDLE;
    }

    /* attempt to do it without extra buffering */
    uint8_t count;
    if (sd_ble_tx_buffer_count_get(&count) == NRF_SUCCESS)
    {
        if (count > 0)
        {
            ble_gatts_hvx_params_t hvx_params;
            hvx_params.handle = m_mesh_service.ble_val_char_handles.value_handle;
            hvx_params.type = BLE_GATT_HVX_NOTIFICATION;
            hvx_params.offset = 0;
            switch (p_gatt_evt->opcode)
            {
                case MESH_GATT_EVT_OPCODE_DATA:
                    hvx_params.p_len = p_gatt_evt->param.data_update.data_len + 4;
                    break;
                case MESH_GATT_EVT_OPCODE_FLAG_SET:
                case MESH_GATT_EVT_OPCODE_FLAG_CLEAR:
                case MESH_GATT_EVT_OPCODE_FLAG_REQ:
                case MESH_GATT_EVT_OPCODE_FLAG_RSP:
                    hvx_params.p_len = 4;
                    break;
                default:
                    hvx_params.p_len = 1;
            }
            hvx_params.p_data = (uint8_t*) p_gatt_evt;

            return sd_ble_gatts_hvx(m_active_conn_handle, CONN_HANDLE_INVALID);
        }
    }

    return NRF_ERROR_BUSY;  
}

static uint32_t mesh_md_char_add(mesh_metadata_char_t* metadata)
{
    if (metadata->mesh_channel > MESH_CHANNEL_MAX ||
        metadata->mesh_interval_min_ms < MESH_INTERVAL_MIN_MAX ||
        metadata->mesh_interval_min_ms > MESH_INTERVAL_MIN_MAX)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    /* cccd for metadata char */
    ble_gatts_attr_md_t cccd_md;

    memset(&cccd_md, 0, sizeof(cccd_md));

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
    cccd_md.vloc = BLE_GATTS_VLOC_STACK;

    /* metadata char */
    ble_gatts_char_md_t ble_char_md;

    memset(&ble_char_md, 0, sizeof(ble_char_md));

    ble_char_md.char_props.read = 1;
    ble_char_md.char_props.notify = 1;

    ble_char_md.p_char_pf = NULL;
    ble_char_md.p_cccd_md = &cccd_md;
    ble_char_md.p_char_user_desc = NULL;
    ble_char_md.p_user_desc_md = NULL;

    /* ATT metadata */
    ble_gatts_attr_md_t ble_attr_md;

    memset(&ble_attr_md, 0, sizeof(ble_attr_md));

    ble_attr_md.read_perm.lv = 1;
    ble_attr_md.read_perm.sm = 1;
    ble_attr_md.write_perm.lv = 1;
    ble_attr_md.write_perm.sm = 1;

    ble_attr_md.rd_auth = 0;
    ble_attr_md.wr_auth = 0;
    ble_attr_md.vlen = 0;
    ble_attr_md.vloc = BLE_GATTS_VLOC_STACK;

    /* ble characteristic UUID */
    ble_uuid_t ble_uuid;

    ble_uuid.type = BLE_UUID_TYPE_BLE;
    ble_uuid.uuid = MESH_MD_CHAR_UUID;

    /* metadata contents */
    uint8_t value_array[MESH_MD_CHAR_LEN];

    memcpy(&value_array[MESH_MD_CHAR_AA_OFFSET],
        &metadata->mesh_access_addr,
        sizeof(metadata->mesh_access_addr));

    memcpy(&value_array[MESH_MD_CHAR_ADV_INT_OFFSET],
        &metadata->mesh_interval_min_ms,
        sizeof(metadata->mesh_interval_min_ms));

    memcpy(&value_array[MESH_MD_CHAR_CH_OFFSET],
        &metadata->mesh_channel,
        sizeof(metadata->mesh_channel));

    /* ble attribute */
    ble_gatts_attr_t ble_attr;

    memset(&ble_attr, 0, sizeof(ble_attr));

    ble_attr.init_len = MESH_MD_CHAR_LEN;
    ble_attr.init_offs = 0;
    ble_attr.max_len = MESH_MD_CHAR_LEN;
    ble_attr.p_uuid = &ble_uuid;
    ble_attr.p_value = value_array;
    ble_attr.p_attr_md = &ble_attr_md;

    /* add characteristic */
    uint32_t error_code = sd_ble_gatts_characteristic_add(
        m_mesh_service.service_handle,
        &ble_char_md,
        &ble_attr,
        &m_mesh_service.ble_md_char_handles);

    if (error_code != NRF_SUCCESS)
    {
        return NRF_ERROR_INTERNAL;
    }

    return NRF_SUCCESS;
}

static uint32_t mesh_value_char_add(void)
{
    /* BLE GATT metadata */
    ble_gatts_char_md_t ble_char_md;

    memset(&ble_char_md, 0, sizeof(ble_char_md));

    ble_char_md.p_char_pf = &ble_char_pf;
    ble_char_md.char_props.write_wo_resp = 1;
    ble_char_md.char_props.notify = 1;

    ble_char_md.p_cccd_md = NULL;
    ble_char_md.p_sccd_md = NULL;
    ble_char_md.p_char_user_desc = NULL;
    ble_char_md.p_user_desc_md = NULL;

    /* ATT metadata */

    ble_gatts_attr_md_t ble_attr_md;

    memset(&ble_attr_md, 0, sizeof(ble_attr_md));

    /* No security is required */
    ble_attr_md.read_perm.lv = 1;
    ble_attr_md.read_perm.sm = 1;
    ble_attr_md.write_perm.lv = 1;
    ble_attr_md.write_perm.sm = 1;

    ble_attr_md.vloc = BLE_GATTS_VLOC_STACK;
    ble_attr_md.rd_auth = 0;
    ble_attr_md.wr_auth = 0;
    ble_attr_md.vlen = 1;

    /* ble characteristic UUID */
    ble_uuid_t ble_uuid;

    ble_uuid.type = m_mesh_base_uuid_type;
    ble_uuid.uuid = MESH_VALUE_CHAR_UUID;

    /* ble attribute */
    ble_gatts_attr_t ble_attr;
    uint8_t default_value = 0;

    memset(&ble_attr, 0, sizeof(ble_attr));

    ble_attr.init_len = 1;
    ble_attr.init_offs = 0;
    ble_attr.max_len = sizeof(mesh_gatt_evt_t);
    ble_attr.p_attr_md = &ble_attr_md;
    ble_attr.p_uuid = &ble_uuid;
    ble_attr.p_value = &default_value;

    /* add to service */
    uint32_t error_code = sd_ble_gatts_characteristic_add(
            m_mesh_service.service_handle,
            &ble_char_md,
            &ble_attr,
            &m_mesh_service.ble_val_char_handles);

    if (error_code != NRF_SUCCESS)
    {
        return NRF_ERROR_INTERNAL;
    }

    return NRF_SUCCESS;
}
/*****************************************************************************
* Interface functions
*****************************************************************************/
uint32_t mesh_gatt_init(uint32_t access_address, uint8_t channel, uint32_t interval_min_ms)
{
    uint32_t error_code;
    mesh_metadata_char_t md_char;
    md_char.mesh_access_addr = access_address;
    md_char.mesh_interval_min_ms = interval_min_ms;
    md_char.mesh_channel = channel;

    error_code = mesh_md_char_add(&md_char);
    if (error_code != NRF_SUCCESS)
    {
        return error_code;
    }

    error_code = mesh_value_char_add();
    if (error_code != NRF_SUCCESS)
    {
        return error_code;
    }

    m_is_initialized = true;

    return NRF_SUCCESS;
}

uint32_t mesh_gatt_value_set(rbc_mesh_value_handle_t handle, uint8_t* data, uint8_t length)
{
    if (length > RBC_MESH_VALUE_MAX_LEN)
    {
        return NRF_ERROR_INVALID_LENGTH;
    }
    if (m_active_conn_handle)
    {
        mesh_gatt_evt_t gatt_evt;
        gatt_evt.opcode = MESH_GATT_EVT_OPCODE_DATA;
        gatt_evt.param.data_update.handle = handle;
        gatt_evt.param.data_update.data_len = length;
        memcpy(gatt_evt.param.data_update.data, data, length);

        return mesh_gatt_evt_push(&gatt_evt);
    }
    else
    {
        return BLE_ERROR_INVALID_CONN_HANDLE;
    }
}

void mesh_gatt_sd_ble_event_handle(ble_evt_t* p_ble_evt)
{
    mesh_gatt_evt_t rsp_evt;
    bool send_response = false;
    if (p_ble_evt->header.evt_id == BLE_GATTS_EVT_WRITE)
    {
        if (p_ble_evt->evt.gatts_evt.write.handle == m_mesh_service.ble_val_char_handles.value_handle)
        {
            uint8_t len = p_ble_evt->evt.gatts_evt.write.len;
            mesh_gatt_evt_t* p_gatt_evt = (mesh_gatt_evt_t*) p_ble_evt->evt.gatts_evt.write.data;
            switch ((mesh_gatt_evt_opcode_t) p_gatt_evt->opcode)
            {
                case MESH_GATT_EVT_OPCODE_DATA:
                    {
                        if (p_gatt_evt->param.data_update.handle > RBC_MESH_VALUE_HANDLE_MAX)
                        {
                            rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE;
                            send_response = true;
                            break;
                        }
                        vh_data_status_t vh_data_status = vh_local_update(
                                p_gatt_evt->param.data_update.handle,
                                p_gatt_evt->param.data_update.data,
                                p_gatt_evt->param.data_update.data_len);
                        if (vh_data_status_t == VH_DATA_STATUS_UNKNOWN)
                        {
                            rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_BUSY;
                            send_response = true;
                            break;
                        }
                    }
                break;

                case MESH_GATT_EVT_OPCODE_FLAG_SET:
                    switch (p_gatt_evt->param.flag_update.flag)
                    {
                        case MESH_GATT_EVT_FLAG_PERSISTENT:
                            if (vh_value_persistence_set(p_gatt_evt->param.flag_update.handle, 
                                        !!(p_gatt_evt->param.flag_update.value))
                                    != NRF_SUCCESS)
                            {
                                rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE;
                                send_response = true;
                            }
                            break;

                        case MESH_GATT_EVT_FLAG_DO_TX:
                            if (p_gatt_evt->param.flag_update.value)
                            {
                                if (vh_value_enable(p_gatt_evt->param.flag_update.handle)
                                        != NRF_SUCCESS)
                                {
                                    rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE;
                                    send_response = true;
                                }
                            }
                            else
                            {
                                if (vh_value_disable(p_gatt_evt->param.flag_update.handle)
                                        != NRF_SUCCESS)
                                {
                                    rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE;
                                    send_response = true;
                                }
                            }
                            break;

                        default:
                            rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_UNKNOWN_FLAG;
                            send_response = true;
                    }
                    break;

               case MESH_GATT_EVT_OPCODE_FLAG_REQ:
                    switch (p_gatt_evt->param.flag_update.flag)
                    {
                        case MESH_GATT_EVT_FLAG_PERSISTENT:
                            {
                                if (p_gatt_evt->param.flag_update.handle == RBC_MESH_INVALID_HANDLE)
                                { 
                                    rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE;
                                    send_response = true;
                                    break;
                                }

                                bool is_persistent = false;
                                if (vh_value_persistence_get(p_gatt_evt->param.flag_update.handle, &is_persistent) 
                                        != NRF_SUCCESS)
                                {
                                    rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_NOT_FOUND;
                                    send_response = true;
                                    break;
                                }

                                rsp_evt.opcode = MESH_GATT_EVT_OPCODE_FLAG_RSP;
                                rsp_evt.param.flag_update.handle = p_gatt_evt->param.flag_update.handle;
                                rsp_evt.param.flag_update.flag = p_gatt_evt->param.flag_update.flag;
                                rsp_evt.param.flag_update.value = (uint8_t) is_persistent;
                                send_response = true;
                            }
                            break;

                        case MESH_GATT_EVT_FLAG_DO_TX:
                            {
                                bool is_enabled;
                                if (vh_value_is_enabled(p_gatt_evt->param.flag_update.handle, &is_enabled)
                                        != NRF_SUCCESS)
                                {
                                    rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_HANDLE;
                                    send_response = true;
                                    break;
                                }

                                rsp_evt.opcode = MESH_GATT_EVT_OPCODE_FLAG_RSP;
                                rsp_evt.param.flag_update.handle = p_gatt_evt->param.flag_update.handle;
                                rsp_evt.param.flag_update.flag = p_gatt_evt->param.flag_update.flag;
                                rsp_evt.param.flag_update.value = (uint8_t) is_enabled;
                                send_response = true;
                            }
                            break;

                        default:
                            rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_UNKNOWN_FLAG;
                            send_response = true;
                    }
                    break;

                default:
                    rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_INVALID_OPCODE;
                    send_response = true;
            }
        }
        else if (p_ble_evt->evt.gatts_evt.write.handle == m_mesh_service.ble_md_char_handles.value_handle)
        {
            mesh_metadata_char_t* p_md = (mesh_metadata_char_t*) p_ble_evt->evt.gatts_evt.write.data;
            tc_radio_params_set(p_md->mesh_access_addr, p_md->mesh_channel);
            if (vh_min_interval_set(p_md->mesh_interval_min_ms) != NRF_SUCCESS)
            {
                rsp_evt.opcode = MESH_GATT_EVT_OPCODE_ERROR_UNKNOWN_FLAG;
                send_response = true;
            }
        }
    }
    mesh_gatt_evt_push(&rsp_evt);
}

uint32_t mesh_gatt_conn_handle_update(uint16_t conn_handle)
{
    m_active_conn_handle = conn_handle;

    return NRF_SUCCESS;
}

uint32_t mesh_gatt_evt_write_handle(ble_gatts_evt_write_t* evt)
{
    return NRF_SUCCESS;
}

