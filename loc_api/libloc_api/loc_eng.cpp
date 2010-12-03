/******************************************************************************
  @file:  loc_eng.cpp
  @brief:

  DESCRIPTION
    This file defines the implemenation for GPS hardware abstraction layer.

  INITIALIZATION AND SEQUENCING REQUIREMENTS

  -----------------------------------------------------------------------------
Copyright (c) 2009, QUALCOMM USA, INC.

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

�         Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer. 

�         Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution. 

�         Neither the name of the QUALCOMM USA, INC.  nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission. 

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  -----------------------------------------------------------------------------

******************************************************************************/

/*=====================================================================
$Header: $
$DateTime: $
$Author: $
======================================================================*/

#define LOG_NDEBUG 0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <rpc/rpc.h>
#include "loc_api_rpc_glue.h"
#include "loc_apicb_appinit.h"

#include <hardware_legacy/gps.h>
#include <cutils/properties.h>
#include <cutils/sched_policy.h>
#include <utils/SystemClock.h>

#include <loc_eng.h>
#include <loc_eng_ni.h>

#define LOG_TAG "lib_locapi"
#include <utils/Log.h>

// comment this out to enable logging
// #undef LOGD
// #define LOGD(...) {}

#define DEBUG_MOCK_NI 0

// Function declarations for sLocEngInterface
static int  loc_eng_init(GpsCallbacks* callbacks);
static int  loc_eng_start();
static int  loc_eng_stop();
static int  loc_eng_set_position_mode(GpsPositionMode mode, int fix_frequency);
static void loc_eng_cleanup();
static int  loc_eng_inject_time(GpsUtcTime time, int64_t timeReference, int uncertainty);
static int  loc_eng_inject_location(double latitude, double longitude, float accuracy);
static void loc_eng_delete_aiding_data (GpsAidingData f);
static const void* loc_eng_get_extension(const char* name);

// Function declarations for sLocEngAGpsInterface
static void loc_eng_agps_init(AGpsCallbacks* callbacks);
static int loc_eng_agps_data_conn_open(const char* apn);
static int loc_eng_agps_data_conn_closed();
static int loc_eng_agps_data_conn_failed();
static int loc_eng_agps_set_server(AGpsType type, const char* hostname, int port);


static int32 loc_event_cb (rpc_loc_client_handle_type client_handle, 
                           rpc_loc_event_mask_type loc_event, 
                           const rpc_loc_event_payload_u_type* loc_event_payload);
static void loc_eng_report_position (const rpc_loc_parsed_position_s_type *location_report_ptr);
static void loc_eng_report_sv (const rpc_loc_gnss_info_s_type *gnss_report_ptr);
static void loc_eng_report_status (const rpc_loc_status_event_s_type *status_report_ptr);
static void loc_eng_report_nmea (const rpc_loc_nmea_report_s_type *nmea_report_ptr);
static void loc_eng_process_conn_request (const rpc_loc_server_request_s_type *server_request_ptr);

static void* loc_eng_process_deferred_action (void* arg);
static void loc_eng_process_atl_deferred_action (boolean data_connection_succeeded,
        boolean data_connection_closed);
static void loc_eng_delete_aiding_data_deferred_action (void);
static int loc_eng_set_gps_lock(rpc_loc_lock_e_type lock_type);
static int set_agps_server();

// Defines the GpsInterface in gps.h
static const GpsInterface sLocEngInterface =
{
    loc_eng_init,
    loc_eng_start,
    loc_eng_stop,
    loc_eng_cleanup,
    loc_eng_inject_time,
    loc_eng_inject_location,
    loc_eng_delete_aiding_data,
    loc_eng_set_position_mode,
    loc_eng_get_extension,
};

static const AGpsInterface sLocEngAGpsInterface =
{
    loc_eng_agps_init,
    loc_eng_agps_data_conn_open,
    loc_eng_agps_data_conn_closed,
    loc_eng_agps_data_conn_failed,
    loc_eng_agps_set_server,
};

// Global data structure for location engine
loc_eng_data_s_type loc_eng_data;

/*===========================================================================
FUNCTION    gps_get_hardware_interface

DESCRIPTION
   Returns the GPS hardware interaface based on LOC API
   if GPS is enabled.

DEPENDENCIES
   None

RETURN VALUE
   0: success

SIDE EFFECTS
   N/A

===========================================================================*/
const GpsInterface* gps_get_hardware_interface ()
{
    char propBuf[PROPERTY_VALUE_MAX];

    // check to see if GPS should be disabled
    property_get("gps.disable", propBuf, "");
    if (propBuf[0] == '1')
    {
        LOGD("gps_get_interface returning NULL because gps.disable=1");
        return NULL;
    }

    return &sLocEngInterface;
}

/*===========================================================================
FUNCTION    loc_eng_init

DESCRIPTION
   Initialize the location engine, this include setting up global datas
   and registers location engien with loc api service.

DEPENDENCIES
   None

RETURN VALUE
   0: success

SIDE EFFECTS
   N/A

===========================================================================*/
static int loc_eng_init(GpsCallbacks* callbacks)
{
    LOGD("loc_eng_init: entered");
    if( loc_eng_data.engine_status != GPS_STATUS_NONE && loc_eng_data.engine_status != GPS_STATUS_ENGINE_OFF ) {

        LOGE("loc_eng_init: engine_status = %d but not GPS_STATUS_NONE or GPS_STATUS_ENGINE_OFF", loc_eng_data.engine_status);
        return -1;
    }
    memset (&loc_eng_data, 0, sizeof (loc_eng_data_s_type));

    // LOC ENG module data initialization
    loc_eng_data.location_cb  = callbacks->location_cb;
    loc_eng_data.sv_status_cb = callbacks->sv_status_cb;
    loc_eng_data.status_cb    = callbacks->status_cb;
    loc_eng_data.nmea_cb    = callbacks->nmea_cb;

    rpc_loc_event_mask_type event = RPC_LOC_EVENT_PARSED_POSITION_REPORT |
                                    RPC_LOC_EVENT_SATELLITE_REPORT |
                                    RPC_LOC_EVENT_LOCATION_SERVER_REQUEST |
                                    RPC_LOC_EVENT_ASSISTANCE_DATA_REQUEST |
                                    RPC_LOC_EVENT_IOCTL_REPORT |
                                    RPC_LOC_EVENT_STATUS_REPORT |
                                    RPC_LOC_EVENT_NMEA_POSITION_REPORT |
                                    RPC_LOC_EVENT_NI_NOTIFY_VERIFY_REQUEST;

    loc_eng_data.work_queue = NULL;
    loc_eng_data.last_fix_time = 0;

    pthread_mutex_init (&(loc_eng_data.deferred_action_mutex), NULL);
    pthread_cond_init  (&(loc_eng_data.deferred_action_cond) , NULL);
    loc_eng_data.deferred_action_thread_need_exit = FALSE;
 
    memset (loc_eng_data.apn_name, 0, sizeof (loc_eng_data.apn_name));

    loc_eng_data.aiding_data_for_deletion = 0;
    loc_eng_data.engine_status = GPS_STATUS_ENGINE_ON;

    // XTRA module data initialization
    loc_eng_data.xtra_module_data.download_request_cb = NULL;
    loc_eng_data.xtra_module_data.download_request_pending = FALSE;
    pthread_mutex_init(&loc_eng_data.xtra_module_data.xtra_mutex, NULL);

    // IOCTL module data initialization
    loc_eng_data.ioctl_data.cb_is_selected  = FALSE;
    loc_eng_data.ioctl_data.cb_is_waiting   = FALSE;
    loc_eng_data.ioctl_data.client_handle   = RPC_LOC_CLIENT_HANDLE_INVALID;
    memset (&(loc_eng_data.ioctl_data.cb_payload),
            0,
            sizeof (rpc_loc_ioctl_callback_s_type));

    pthread_mutex_init (&(loc_eng_data.ioctl_data.cb_data_mutex), NULL);
    pthread_cond_init(&loc_eng_data.ioctl_data.cb_arrived_cond, NULL);

    loc_eng_data.deferred_action_thread = NULL;
    pthread_create (&(loc_eng_data.deferred_action_thread),
                    NULL,
                    loc_eng_process_deferred_action,
                    NULL);
    sleep(2);
    // Start the LOC api RPC service
    loc_api_glue_init ();
    // open client
    loc_eng_data.client_handle = loc_open (event, loc_event_cb);
    //disable GPS lock
    loc_eng_set_gps_lock(RPC_LOC_LOCK_NONE);

    LOGD("loc_eng_init: called, client id = %ld", loc_eng_data.client_handle);
    return 0;
}

/*===========================================================================
FUNCTION    loc_eng_cleanup

DESCRIPTION
   Cleans location engine. The location client handle will be released.

DEPENDENCIES
   None

RETURN VALUE
   None

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_cleanup()
{

    work_item *work;

    if (loc_eng_data.deferred_action_thread)
    {
        /* Terminate deferred action working thread */
        pthread_mutex_lock (&loc_eng_data.deferred_action_mutex);
        loc_eng_data.deferred_action_thread_need_exit = TRUE;
        pthread_cond_signal  (&loc_eng_data.deferred_action_cond);
        pthread_mutex_unlock (&loc_eng_data.deferred_action_mutex);

        void* ignoredValue;
        pthread_join(loc_eng_data.deferred_action_thread, &ignoredValue);
        loc_eng_data.deferred_action_thread = NULL;
    }

    // clean up
    (void) loc_close (loc_eng_data.client_handle);

    // lock the queue
    pthread_mutex_lock(&loc_eng_data.deferred_action_mutex);
    // free any remaining work items
    while(loc_eng_data.work_queue) {
        work = loc_eng_data.work_queue;
        loc_eng_data.work_queue = work->next;
        free(work);
    }
    // unlock the queue
    pthread_mutex_unlock(&loc_eng_data.deferred_action_mutex);

    pthread_mutex_destroy (&loc_eng_data.xtra_module_data.xtra_mutex);

    pthread_mutex_destroy (&loc_eng_data.deferred_action_mutex);
    pthread_cond_destroy  (&loc_eng_data.deferred_action_cond);

    pthread_mutex_destroy (&loc_eng_data.ioctl_data.cb_data_mutex);
    pthread_cond_destroy  (&loc_eng_data.ioctl_data.cb_arrived_cond);

    // RPC glue code
    loc_api_glue_deinit();

    loc_eng_data.engine_status = GPS_STATUS_ENGINE_OFF;
}


/*===========================================================================
FUNCTION    loc_eng_start

DESCRIPTION
   Starts the tracking session

DEPENDENCIES
   None

RETURN VALUE
   0: success

SIDE EFFECTS
   N/A

===========================================================================*/
static int loc_eng_start()
{
    int ret_val;
    LOGD("loc_eng_start");

    if (loc_eng_data.position_mode != GPS_POSITION_MODE_STANDALONE &&
            loc_eng_data.agps_server_host[0] != 0 &&
            loc_eng_data.agps_server_port != 0) {
        int result = set_agps_server();
        LOGD("loc_eng_start: set_agps_server returned = %d", result);
    }

    ret_val = loc_start_fix (loc_eng_data.client_handle);

    if (ret_val != RPC_LOC_API_SUCCESS)
    {
        LOGD("loc_eng_start: returned error = %d", ret_val);
    }

    return 0;
}


/*===========================================================================
FUNCTION    loc_eng_stop

DESCRIPTION
   Stops the tracking session

DEPENDENCIES
   None

RETURN VALUE
   0: success

SIDE EFFECTS
   N/A

===========================================================================*/
static int loc_eng_stop()
{
    int ret_val;

    LOGD("loc_eng_stop");

    ret_val = loc_stop_fix (loc_eng_data.client_handle);
    if (ret_val != RPC_LOC_API_SUCCESS)
    {
        LOGD("loc_eng_stop: returned error = %d", ret_val);
    }

    // send_delete_aiding_data must be done when GPS engine is off
    if (loc_eng_data.aiding_data_for_deletion != 0)
    {
        if(loc_eng_data.engine_status == GPS_STATUS_SESSION_BEGIN)
            LOGE("loc_eng_stop: expected engine_status != GPS_STATUS_SESSION_BEGIN after loc_stop_fix");

        LOGD("loc_eng_stop: engine_status = %d, starting loc_eng_delete_aiding_data_deferred_action", loc_eng_data.engine_status);
        loc_eng_delete_aiding_data_deferred_action ();
    }

    return 0;
}

static int loc_eng_set_gps_lock(rpc_loc_lock_e_type lock_type)
{
    rpc_loc_ioctl_data_u_type    ioctl_data;
    boolean                      ret_val;

    LOGD("loc_eng_set_gps_lock: client = %ld, lock_type = %d",
            loc_eng_data.client_handle, lock_type);

    ioctl_data.rpc_loc_ioctl_data_u_type_u.engine_lock = lock_type;
    ioctl_data.disc = RPC_LOC_IOCTL_SET_ENGINE_LOCK;

    ret_val = loc_eng_ioctl (loc_eng_data.client_handle,
                            RPC_LOC_IOCTL_SET_ENGINE_LOCK,
                            &ioctl_data,
                            LOC_IOCTL_DEFAULT_TIMEOUT,
                            NULL /* No output information is expected*/);

    if (ret_val != TRUE)
    {
        LOGD("loc_eng_set_gps_lock: failed");
    }

    return 0;
}

/*===========================================================================
FUNCTION    loc_eng_set_position_mode

DESCRIPTION
   Sets the mode and fix frequnecy (in seconds) for the tracking session.

DEPENDENCIES
   None

RETURN VALUE
   0: success

SIDE EFFECTS
   N/A

===========================================================================*/
static int loc_eng_set_position_mode(GpsPositionMode mode, int fix_frequency)
{
    rpc_loc_ioctl_data_u_type    ioctl_data;
    rpc_loc_fix_criteria_s_type *fix_criteria_ptr;
    boolean                      ret_val;

    LOGD("loc_eng_set_position_mode: client = %ld, interval = %d, mode = %d",
            loc_eng_data.client_handle, fix_frequency, mode);

    loc_eng_data.position_mode = mode;
    ioctl_data.disc = RPC_LOC_IOCTL_SET_FIX_CRITERIA;

    fix_criteria_ptr = &(ioctl_data.rpc_loc_ioctl_data_u_type_u.fix_criteria);
    fix_criteria_ptr->valid_mask = RPC_LOC_FIX_CRIT_VALID_MIN_INTERVAL |
                                   RPC_LOC_FIX_CRIT_VALID_PREFERRED_OPERATION_MODE |
                                   RPC_LOC_FIX_CRIT_VALID_RECURRENCE_TYPE;
    fix_criteria_ptr->min_interval = fix_frequency * 1000; // Translate to ms
    fix_criteria_ptr->recurrence_type = RPC_LOC_PERIODIC_FIX;

    if (mode == GPS_POSITION_MODE_MS_BASED)
    {
        fix_criteria_ptr->preferred_operation_mode = RPC_LOC_OPER_MODE_MSB;
    }
    else if (mode == GPS_POSITION_MODE_MS_ASSISTED)
    {
        fix_criteria_ptr->preferred_operation_mode = RPC_LOC_OPER_MODE_MSA;
    }
    // Default: standalone
    else
    {
        fix_criteria_ptr->preferred_operation_mode = RPC_LOC_OPER_MODE_STANDALONE;
    }

    ret_val = loc_eng_ioctl(loc_eng_data.client_handle,
                            RPC_LOC_IOCTL_SET_FIX_CRITERIA,
                            &ioctl_data,
                            LOC_IOCTL_DEFAULT_TIMEOUT,
                            NULL /* No output information is expected*/);

    if (ret_val != TRUE)
    {
        LOGD("loc_eng_set_position_mode: failed");
    }

    return 0;
}

/*===========================================================================
FUNCTION    loc_eng_inject_time

DESCRIPTION
   This is used by Java native function to do time injection.

DEPENDENCIES
   None

RETURN VALUE
   RPC_LOC_API_SUCCESS

SIDE EFFECTS
   N/A

===========================================================================*/
static int loc_eng_inject_time (GpsUtcTime time, int64_t timeReference, int uncertainty)
{
    rpc_loc_ioctl_data_u_type       ioctl_data;
    rpc_loc_assist_data_time_s_type *time_info_ptr;
    boolean                          ret_val;

    LOGD("loc_eng_inject_time: uncertainty = %d", uncertainty);

    ioctl_data.disc = RPC_LOC_IOCTL_INJECT_UTC_TIME;

    time_info_ptr = &(ioctl_data.rpc_loc_ioctl_data_u_type_u.assistance_data_time);
    time_info_ptr->time_utc = time;
    time_info_ptr->time_utc += (int64_t)(android::elapsedRealtime() - timeReference);
    time_info_ptr->uncertainty = uncertainty; // Uncertainty in ms

    ret_val = loc_eng_ioctl (loc_eng_data.client_handle,
                             RPC_LOC_IOCTL_INJECT_UTC_TIME,
                             &ioctl_data,
                             LOC_IOCTL_DEFAULT_TIMEOUT,
                             NULL /* No output information is expected*/);

    if (ret_val != TRUE)
    {
        LOGD("loc_eng_inject_time: failed");
    }

    return 0;
}

static int loc_eng_inject_location (double latitude, double longitude, float accuracy)
{
    boolean                         ret_val;
    rpc_loc_ioctl_data_u_type       ioctl_data;
    rpc_loc_assist_data_pos_s_type *pos_info_ptr;

    LOGD("loc_eng_inject_location: lat=%f, lon=%f, acc=%f", latitude, longitude, accuracy);

    ioctl_data.disc = RPC_LOC_IOCTL_INJECT_UTC_TIME;

    pos_info_ptr = &(ioctl_data.rpc_loc_ioctl_data_u_type_u.assistance_data_position);
    memset(pos_info_ptr, 0, sizeof(rpc_loc_assist_data_pos_s_type));

    pos_info_ptr->valid_mask = RPC_LOC_ASSIST_POS_VALID_LATITUDE |
                               RPC_LOC_ASSIST_POS_VALID_LONGITUDE |
                               RPC_LOC_ASSIST_POS_VALID_HOR_UNC_CIRCULAR;
    pos_info_ptr->latitude  = latitude;
    pos_info_ptr->longitude = longitude;
    pos_info_ptr->hor_unc_circular = accuracy;

    ret_val = loc_eng_ioctl (loc_eng_data.client_handle, RPC_LOC_IOCTL_INJECT_POSITION,
                             &ioctl_data, LOC_IOCTL_DEFAULT_TIMEOUT, NULL);

    if (ret_val != TRUE) {
        LOGD("loc_eng_inject_location: failed");
    }

    return 0;
}

/*===========================================================================
FUNCTION    loc_eng_delete_aiding_data

DESCRIPTION
   This is used by Java native function to delete the aiding data. The function
   updates the global variable for the aiding data to be deleted. If the GPS
   engine is off, the aiding data will be deleted. Otherwise, the actual action
   will happen when gps engine is turned off.

DEPENDENCIES
   Assumes the aiding data type specified in GpsAidingData matches with
   LOC API specification.

RETURN VALUE
   RPC_LOC_API_SUCCESS

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_delete_aiding_data (GpsAidingData f)
{
    // If this is DELETE ALL
    if (f == GPS_DELETE_ALL)
    {
        loc_eng_data.aiding_data_for_deletion = GPS_DELETE_ALL;
    }
    else
    {
        // Currently, LOC API only support deletion of all aiding data,
        // since the Android defined aiding data mask matches with modem,
        // so just pass them down without any translation
        loc_eng_data.aiding_data_for_deletion |= f;
    }

    if ((loc_eng_data.engine_status != GPS_STATUS_SESSION_BEGIN) &&
        (loc_eng_data.aiding_data_for_deletion != 0))
    {
        LOGD("loc_eng_delete_aiding_data: engine_status = %d, starting loc_eng_delete_aiding_data_deferred_action", loc_eng_data.engine_status);
        loc_eng_delete_aiding_data_deferred_action ();
    }
    /* if the the status is GPS_STATUS_SESSION_BEGIN, we will defer this until loc_eng_stop() */
}

/*===========================================================================
FUNCTION    loc_eng_get_extension

DESCRIPTION
   Get the gps extension to support XTRA.

DEPENDENCIES
   N/A

RETURN VALUE
   The GPS extension interface.

SIDE EFFECTS
   N/A

===========================================================================*/
static const void* loc_eng_get_extension(const char* name)
{
    if (strcmp(name, GPS_XTRA_INTERFACE) == 0)
    {
        return &sLocEngXTRAInterface;
    }
    else if (strcmp(name, AGPS_INTERFACE) == 0)
    {
        return &sLocEngAGpsInterface;
    }
    else if (strcmp(name, GPS_NI_INTERFACE) == 0)
    {
        return &sLocEngNiInterface;
    }

    return NULL;
}

#if DEBUG_MOCK_NI == 1
/*===========================================================================
FUNCTION    mock_ni

DESCRIPTION
   DEBUG tool: simulate an NI request

DEPENDENCIES
   N/A

RETURN VALUE
   None

SIDE EFFECTS
   N/A

===========================================================================*/
static void* mock_ni(void* arg)
{
    static int busy = 0;

    if (busy) return NULL;

    busy = 1;

    sleep(5);

    rpc_loc_client_handle_type           client_handle;
    rpc_loc_event_mask_type              loc_event;
    rpc_loc_event_payload_u_type         payload;
    rpc_loc_ni_event_s_type             *ni_req;
    rpc_loc_ni_supl_notify_verify_req_s_type *supl_req;

    client_handle = (rpc_loc_client_handle_type) arg;

    loc_event = RPC_LOC_EVENT_NI_NOTIFY_VERIFY_REQUEST;
    payload.disc = loc_event;

    ni_req = &payload.rpc_loc_event_payload_u_type_u.ni_request;
    ni_req->event = RPC_LOC_NI_EVENT_SUPL_NOTIFY_VERIFY_REQ;
    supl_req = &ni_req->payload.rpc_loc_ni_event_payload_u_type_u.supl_req;

    // Encodings for Spirent Communications
    char client_name[80]  = {0x53,0x78,0x5A,0x5E,0x76,0xD3,0x41,0xC3,0x77,
            0xBB,0x5D,0x77,0xA7,0xC7,0x61,0x7A,0xFA,0xED,0x9E,0x03};
    char requestor_id[80] = {0x53,0x78,0x5A,0x5E,0x76,0xD3,0x41,0xC3,0x77,
            0xBB,0x5D,0x77,0xA7,0xC7,0x61,0x7A,0xFA,0xED,0x9E,0x03};

    supl_req->flags = RPC_LOC_NI_CLIENT_NAME_PRESENT |
                      RPC_LOC_NI_REQUESTOR_ID_PRESENT |
                      RPC_LOC_NI_ENCODING_TYPE_PRESENT;

    supl_req->datacoding_scheme = RPC_LOC_NI_SUPL_GSM_DEFAULT;

    supl_req->client_name.data_coding_scheme = RPC_LOC_NI_SUPL_GSM_DEFAULT; // no coding
    supl_req->client_name.client_name_string.client_name_string_len = strlen(client_name);
    supl_req->client_name.client_name_string.client_name_string_val = client_name;
    supl_req->client_name.string_len = strlen(client_name);

    supl_req->requestor_id.data_coding_scheme = RPC_LOC_NI_SUPL_GSM_DEFAULT;
    supl_req->requestor_id.requestor_id_string.requestor_id_string_len = strlen(requestor_id);
    supl_req->requestor_id.requestor_id_string.requestor_id_string_val = requestor_id;
    supl_req->requestor_id.string_len = strlen(requestor_id);

    supl_req->notification_priv_type = RPC_LOC_NI_USER_NOTIFY_VERIFY_ALLOW_NO_RESP;
    supl_req->user_response_timer = 10;

    loc_event_cb(client_handle, loc_event, &payload);

    busy = 0;

    return NULL;
}
#endif // DEBUG_MOCK_NI

/*===========================================================================
FUNCTION    loc_event_cb

DESCRIPTION
   This is the callback function registered by loc_open.

DEPENDENCIES
   N/A

RETURN VALUE
   RPC_LOC_API_SUCCESS

SIDE EFFECTS
   N/A

===========================================================================*/
static int32 loc_event_cb(
    rpc_loc_client_handle_type           client_handle,
    rpc_loc_event_mask_type              loc_event,
    const rpc_loc_event_payload_u_type*  loc_event_payload
    )
{

    work_item *work, *queue;

    LOGV("loc_event_cb: client = %ld, loc_event = 0x%llx", client_handle, loc_event);
    if (client_handle == loc_eng_data.client_handle)
    {
        // create work queue item
        work = (work_item*)malloc(sizeof(work_item));
        work->next = NULL;
        work->loc_event = loc_event;
        memcpy(&work->loc_event_payload, loc_event_payload, sizeof(*loc_event_payload));
        // lock the queue
        pthread_mutex_lock(&loc_eng_data.deferred_action_mutex);
        // add item to end of queue
        if (!loc_eng_data.work_queue) loc_eng_data.work_queue = work;
        else {
            queue = loc_eng_data.work_queue;
            while(queue->next) queue = queue->next;
            queue->next = work;
        }
        // signal deferred action thread
        pthread_cond_signal  (&loc_eng_data.deferred_action_cond);
        pthread_mutex_unlock (&loc_eng_data.deferred_action_mutex);
    }
    else
    {
        LOGD("loc_event_cb: client mismatch: received = %ld, expected = %ld", client_handle, loc_eng_data.client_handle);
    }

    return RPC_LOC_API_SUCCESS;
}

/*===========================================================================
FUNCTION    loc_eng_report_position

DESCRIPTION
   Reports position information to the Java layer.

DEPENDENCIES
   N/A

RETURN VALUE
   N/A

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_report_position (const rpc_loc_parsed_position_s_type *location_report_ptr)
{
    GpsLocation location;

    LOGV("loc_eng_report_position: location report, valid mask = 0x%llx, sess status = %d",
         location_report_ptr->valid_mask, location_report_ptr->session_status);

    memset (&location, 0, sizeof (GpsLocation));
    if (location_report_ptr->valid_mask & RPC_LOC_POS_VALID_SESSION_STATUS)
    {
        // Not a position report, return
        if (location_report_ptr->session_status == RPC_LOC_SESS_STATUS_SUCCESS)
        {
            if (location_report_ptr->valid_mask & RPC_LOC_POS_VALID_TIMESTAMP_UTC)
            {
                location.timestamp = location_report_ptr->timestamp_utc;
            }

            if ((location_report_ptr->valid_mask & RPC_LOC_POS_VALID_LATITUDE) &&
                (location_report_ptr->valid_mask & RPC_LOC_POS_VALID_LONGITUDE))
            {
                location.flags    |= GPS_LOCATION_HAS_LAT_LONG;
                location.latitude  = location_report_ptr->latitude;
                location.longitude = location_report_ptr->longitude;

                // remember when we got this fix
                loc_eng_data.last_fix_time = android::elapsedRealtime();
            }

            if (location_report_ptr->valid_mask &  RPC_LOC_POS_VALID_ALTITUDE_WRT_ELLIPSOID )
            {
                location.flags    |= GPS_LOCATION_HAS_ALTITUDE;
                location.altitude = location_report_ptr->altitude_wrt_ellipsoid;
            }

            if ((location_report_ptr->valid_mask & RPC_LOC_POS_VALID_SPEED_HORIZONTAL) &&
                (location_report_ptr->valid_mask & RPC_LOC_POS_VALID_SPEED_VERTICAL))
            {
                location.flags    |= GPS_LOCATION_HAS_SPEED;
                location.speed = sqrt(location_report_ptr->speed_horizontal * location_report_ptr->speed_horizontal +
                                     location_report_ptr->speed_vertical * location_report_ptr->speed_vertical);
            }

            if (location_report_ptr->valid_mask &  RPC_LOC_POS_VALID_HEADING)
            {
                location.flags    |= GPS_LOCATION_HAS_BEARING;
                location.bearing = location_report_ptr->heading;
            }

            if (location_report_ptr->valid_mask & RPC_LOC_POS_VALID_HOR_UNC_CIRCULAR)
            {
                location.flags    |= GPS_LOCATION_HAS_ACCURACY;
                location.accuracy = location_report_ptr->hor_unc_circular;
            }

            if (loc_eng_data.location_cb != NULL)
            {
                LOGV("loc_eng_report_position: fire callback");
                loc_eng_data.location_cb (&location);
            }
        }
        else
        {
            LOGV("loc_eng_report_position: ignore position report when session status = %d (2 means GENERAL_FAILURE)", location_report_ptr->session_status);
        }
    }
    else
    {
        LOGV("loc_eng_report_position: ignore position report when session status is not set");
    }
}

/*===========================================================================
FUNCTION    loc_eng_report_sv

DESCRIPTION
   Reports GPS satellite information to the Java layer.

DEPENDENCIES
   N/A

RETURN VALUE
   N/A

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_report_sv (const rpc_loc_gnss_info_s_type *gnss_report_ptr)
{
    GpsSvStatus     SvStatus;
    int             num_svs_max, i;
    const rpc_loc_sv_info_s_type *sv_info_ptr;

    LOGV("loc_eng_report_sv: valid_mask = 0x%lx, num of sv = %d",
            gnss_report_ptr->valid_mask, gnss_report_ptr->sv_count);

    memset (&SvStatus, 0, sizeof (GpsSvStatus));
    SvStatus.num_svs = 0;

    num_svs_max = 0;
    if (gnss_report_ptr->valid_mask & RPC_LOC_GNSS_INFO_VALID_SV_COUNT)
    {
        num_svs_max = gnss_report_ptr->sv_count;
        if (num_svs_max > GPS_MAX_SVS)
        {
            num_svs_max = GPS_MAX_SVS;
        }
    }

    if (gnss_report_ptr->valid_mask & RPC_LOC_GNSS_INFO_VALID_SV_LIST)
    {
        for (i = 0; i < num_svs_max; i++)
        {
            sv_info_ptr = &(gnss_report_ptr->sv_list.sv_list_val[i]);

            LOGV("vm=0x%lx, sys=%d, prn=%d, hs=%d, ps=%d, eph=%d, alm=%d, el=%f, az=%f, snr=%f", sv_info_ptr->valid_mask, sv_info_ptr->system, sv_info_ptr->prn, sv_info_ptr->health_status,
                sv_info_ptr->process_status, sv_info_ptr->has_eph, sv_info_ptr->has_alm, sv_info_ptr->elevation, sv_info_ptr->azimuth, sv_info_ptr->snr);

            if (sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_SYSTEM)
            {
                if (sv_info_ptr->system == RPC_LOC_SV_SYSTEM_GPS)
                {
                    SvStatus.sv_list[SvStatus.num_svs].prn = sv_info_ptr->prn;

                    // We only have the data field to report gps eph and alm mask
                    if ((sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_HAS_EPH) &&
                        (sv_info_ptr->has_eph == 1))
                    {
                        SvStatus.ephemeris_mask |= (1 << (sv_info_ptr->prn-1));
                    }

                    if ((sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_HAS_ALM) &&
                        (sv_info_ptr->has_alm == 1))
                    {
                        SvStatus.almanac_mask |= (1 << (sv_info_ptr->prn-1));
                    }

                    if ((sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_PROCESS_STATUS) &&
                        (sv_info_ptr->process_status == RPC_LOC_SV_STATUS_TRACK))
                    {
                        SvStatus.used_in_fix_mask |= (1 << (sv_info_ptr->prn-1));
                    }
                }
                // SBAS: GPS RPN: 120-151,
                // In exteneded measurement report, we follow nmea standard, which is from 33-64.
                else if (sv_info_ptr->system == RPC_LOC_SV_SYSTEM_SBAS)
                {
                    SvStatus.sv_list[SvStatus.num_svs].prn = sv_info_ptr->prn + 33 - 120;
                }
                // Gloness: Slot id: 1-32
                // In extended measurement report, we follow nmea standard, which is 65-96
                else if (sv_info_ptr->system == RPC_LOC_SV_SYSTEM_GLONASS)
                {
                    SvStatus.sv_list[SvStatus.num_svs].prn = sv_info_ptr->prn + (65-1);
                }
                // Unsupported SV system
                else
                {
                    continue;
                }
            } else {
                continue;
            }

            if (sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_SNR)
            {
                SvStatus.sv_list[SvStatus.num_svs].snr = sv_info_ptr->snr;
            }

            if (sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_ELEVATION)
            {
                SvStatus.sv_list[SvStatus.num_svs].elevation = sv_info_ptr->elevation;
            }

            if (sv_info_ptr->valid_mask & RPC_LOC_SV_INFO_VALID_AZIMUTH)
            {
                SvStatus.sv_list[SvStatus.num_svs].azimuth = sv_info_ptr->azimuth;
            }

            SvStatus.num_svs++;
        }
    }

    // hack to work around fact that device does not report which sats are used in the fix
    // some apps don't accept the fix if it they think it came from enough sats
    if ((SvStatus.used_in_fix_mask == 0) && (android::elapsedRealtime() < loc_eng_data.last_fix_time + 10000)) {
        SvStatus.used_in_fix_mask = SvStatus.ephemeris_mask;
    }

    LOGV("loc_eng_report_sv: num_svs = %d, eph mask = 0x%x, alm mask = 0x%x, fix mask = 0x%x",
          SvStatus.num_svs, SvStatus.ephemeris_mask, SvStatus.almanac_mask, SvStatus.used_in_fix_mask);
    if (loc_eng_data.sv_status_cb != NULL)
    {
        loc_eng_data.sv_status_cb(&SvStatus);
    }
}

/*===========================================================================
FUNCTION    loc_eng_report_status

DESCRIPTION
   Reports GPS engine state to Java layer.

DEPENDENCIES
   N/A

RETURN VALUE
   N/A

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_report_status (const rpc_loc_status_event_s_type *status_report_ptr)
{
    GpsStatus status;

    LOGV("loc_eng_report_status: event = %d", status_report_ptr->event);

    memset (&status, 0, sizeof (GpsStatus));
    status.status = GPS_STATUS_NONE;
    if (status_report_ptr->event == RPC_LOC_STATUS_EVENT_ENGINE_STATE)
    {
        if (status_report_ptr->payload.rpc_loc_status_event_payload_u_type_u.engine_state == RPC_LOC_ENGINE_STATE_ON)
        {
            LOGV("loc_eng_report_status: status = GPS_STATUS_SESSION_BEGIN");
            // GPS_STATUS_SESSION_BEGIN implies GPS_STATUS_ENGINE_ON
            status.status = GPS_STATUS_SESSION_BEGIN;
            loc_eng_data.status_cb (&status);
        }
        else if (status_report_ptr->payload.rpc_loc_status_event_payload_u_type_u.engine_state == RPC_LOC_ENGINE_STATE_OFF)
        {
            LOGV("loc_eng_report_status: status = GPS_STATUS_SESSION_END");
            // GPS_STATUS_SESSION_END implies GPS_STATUS_ENGINE_OFF
            status.status = GPS_STATUS_ENGINE_OFF;
            loc_eng_data.status_cb (&status);
        }
        else
        {
            LOGE("loc_eng_report_status: unhandled status %d", status_report_ptr->payload.rpc_loc_status_event_payload_u_type_u.engine_state);
        }
    }
    loc_eng_data.engine_status = status.status;
}

static void loc_eng_report_nmea (const rpc_loc_nmea_report_s_type *nmea_report_ptr)
{
    LOGV("loc_eng_report_nmea: entered");

    if (loc_eng_data.nmea_cb != NULL)
    {
        struct timeval tv;

        gettimeofday(&tv, (struct timezone *) NULL);
        long long now = tv.tv_sec * 1000LL + tv.tv_usec / 1000;

        loc_eng_data.nmea_cb(now, nmea_report_ptr->nmea_sentences.nmea_sentences_val,
                nmea_report_ptr->nmea_sentences.nmea_sentences_len);
    }
}

/*===========================================================================
FUNCTION    loc_eng_process_conn_request

DESCRIPTION
   Requests data connection to be brought up/tore down with the location server.

DEPENDENCIES
   N/A

RETURN VALUE
   N/A

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_process_conn_request (const rpc_loc_server_request_s_type *server_request_ptr)
{
    LOGD("loc_eng_process_conn_request: get loc event location server request, event=%d, conn_handle=%lu",
        server_request_ptr->event, server_request_ptr->payload.rpc_loc_server_request_u_type_u.open_req.conn_handle);

    // This implemenation is based on the fact that modem now at any time has only one data connection for AGPS at any given time
    if (server_request_ptr->event == RPC_LOC_SERVER_REQUEST_OPEN)
    {
        loc_eng_data.conn_handle = server_request_ptr->payload.rpc_loc_server_request_u_type_u.open_req.conn_handle;
        loc_eng_data.agps_status = GPS_REQUEST_AGPS_DATA_CONN;
    }
    else if (server_request_ptr->event == RPC_LOC_SERVER_REQUEST_CLOSE)
    {
        loc_eng_data.conn_handle = server_request_ptr->payload.rpc_loc_server_request_u_type_u.close_req.conn_handle;
        loc_eng_data.agps_status = GPS_RELEASE_AGPS_DATA_CONN;
    }
}

/*===========================================================================
FUNCTION    loc_eng_agps_init

DESCRIPTION


DEPENDENCIES
   NONE

RETURN VALUE
   0

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_agps_init(AGpsCallbacks* callbacks)
{
    LOGV("loc_eng_agps_init");
    loc_eng_data.agps_status_cb = callbacks->status_cb;
}

static int loc_eng_agps_data_conn_open(const char* apn)
{
    size_t apn_len;
    LOGD("loc_eng_agps_data_conn_open: %s", apn);

    if (apn != NULL)
    {
        apn_len = strlen (apn);

        if (apn_len >= sizeof(loc_eng_data.apn_name))
        {
            LOGD("loc_eng_agps_data_conn_open: error, apn name exceeds maximum lenght of 100 chars");
            apn_len = sizeof(loc_eng_data.apn_name) - 1;
        }

        memcpy (loc_eng_data.apn_name, apn, apn_len);
        loc_eng_data.apn_name[apn_len] = '\0';
    }

    loc_eng_process_atl_deferred_action(TRUE, FALSE);
    return 0;
}

static int loc_eng_agps_data_conn_closed()
{
    LOGD("loc_eng_agps_data_conn_closed");
    loc_eng_process_atl_deferred_action(FALSE, TRUE);
    return 0;
}

static int loc_eng_agps_data_conn_failed()
{
    LOGD("loc_eng_agps_data_conn_failed");

    loc_eng_process_atl_deferred_action(FALSE, FALSE);
    return 0;
}

static int set_agps_server()
{
    rpc_loc_ioctl_data_u_type       ioctl_data;
    rpc_loc_server_info_s_type      *server_info_ptr;
    boolean                         ret_val;
    uint16                          port_temp;
    char                            url[24];
    int                             len;
    unsigned char                   *b_ptr;

    if (loc_eng_data.agps_server_host[0] == 0 || loc_eng_data.agps_server_port == 0)
        return -1;

    if (loc_eng_data.agps_server_address == 0) {
        struct hostent* he = gethostbyname(loc_eng_data.agps_server_host);
        if (he)
            loc_eng_data.agps_server_address = *(uint32_t *)he->h_addr_list[0];
    }
    if (loc_eng_data.agps_server_address == 0)
        return -1;

    b_ptr = (unsigned char*) (&loc_eng_data.agps_server_address);
    memset(url, 0, sizeof(url));
    snprintf(url, sizeof(url) - 1, "%d.%d.%d.%d:%d",
            (*(b_ptr + 0)  & 0x000000ff), (*(b_ptr+1) & 0x000000ff),
            (*(b_ptr + 2)  & 0x000000ff), (*(b_ptr+3) & 0x000000ff),
            (loc_eng_data.agps_server_port & (0x0000ffff)));
    len = strlen (url);

    server_info_ptr = &(ioctl_data.rpc_loc_ioctl_data_u_type_u.server_addr);
    ioctl_data.disc = RPC_LOC_IOCTL_SET_UMTS_SLP_SERVER_ADDR;
    server_info_ptr->addr_type = RPC_LOC_SERVER_ADDR_URL;
    server_info_ptr->addr_info.disc =  RPC_LOC_SERVER_ADDR_URL;
    server_info_ptr->addr_info.rpc_loc_server_addr_u_type_u.url.length = len;
    server_info_ptr->addr_info.rpc_loc_server_addr_u_type_u.url.addr.addr_val = url;
    server_info_ptr->addr_info.rpc_loc_server_addr_u_type_u.url.addr.addr_len= len;

    LOGD("set_agps_server: addr = %s", server_info_ptr->addr_info.rpc_loc_server_addr_u_type_u.url.addr.addr_val);

    ret_val = loc_eng_ioctl (loc_eng_data.client_handle,
                            RPC_LOC_IOCTL_SET_UMTS_SLP_SERVER_ADDR,
                            &ioctl_data,
                            LOC_IOCTL_DEFAULT_TIMEOUT,
                            NULL /* No output information is expected*/);

    if (ret_val != TRUE)
    {
        LOGD("set_agps_server: failed");
        return -1;
    }
    else
    {
        LOGV("set_agps_server: successful");
        return 0;
    }
}

static int loc_eng_agps_set_server(AGpsType type, const char* hostname, int port)
{
    LOGD("loc_eng_agps_set_server: type = %d, hostname = %s, port = %d", type, hostname, port);

    if (type != AGPS_TYPE_SUPL)
        return -1;

    strncpy(loc_eng_data.agps_server_host, hostname, sizeof(loc_eng_data.agps_server_host) - 1);
    loc_eng_data.agps_server_port = port;
    return 0;
}

/*===========================================================================
FUNCTION    loc_eng_delete_aiding_data_deferred_action

DESCRIPTION
   This is used to remove the aiding data when GPS engine is off.

DEPENDENCIES
   Assumes the aiding data type specified in GpsAidingData matches with
   LOC API specification.

RETURN VALUE
   RPC_LOC_API_SUCCESS

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_delete_aiding_data_deferred_action (void)
{
    // Currently, we only support deletion of all aiding data,
    // since the Android defined aiding data mask matches with modem,
    // so just pass them down without any translation
    rpc_loc_ioctl_data_u_type          ioctl_data;
    rpc_loc_assist_data_delete_s_type  *assist_data_ptr;
    boolean                             ret_val;

    ioctl_data.disc = RPC_LOC_IOCTL_DELETE_ASSIST_DATA;

    assist_data_ptr = &(ioctl_data.rpc_loc_ioctl_data_u_type_u.assist_data_delete);
    if (loc_eng_data.aiding_data_for_deletion == GPS_DELETE_ALL)
    {
        assist_data_ptr->type = RPC_LOC_ASSIST_DATA_ALL;
    }
    else
    {
        assist_data_ptr->type = loc_eng_data.aiding_data_for_deletion;
    }
    memset (&(assist_data_ptr->reserved), 0, sizeof (assist_data_ptr->reserved));

    ret_val = loc_eng_ioctl (loc_eng_data.client_handle,
                             RPC_LOC_IOCTL_DELETE_ASSIST_DATA ,
                             &ioctl_data,
                             LOC_IOCTL_DEFAULT_TIMEOUT,
                             NULL);

    loc_eng_data.aiding_data_for_deletion = 0;
    LOGD("loc_eng_delete_aiding_data_deferred_action: loc_eng_ioctl for aiding data deletion returned %d, 1 for success", ret_val);
}

/*===========================================================================
FUNCTION    loc_eng_process_atl_deferred_action

DESCRIPTION
   This is used to inform the location engine of the processing status for
   data connection open/close request.

DEPENDENCIES
   None

RETURN VALUE
   RPC_LOC_API_SUCCESS

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_process_atl_deferred_action (boolean data_connection_succeeded,
        boolean data_connection_closed)
{
    rpc_loc_server_open_status_s_type  *conn_open_status_ptr;
    rpc_loc_server_close_status_s_type *conn_close_status_ptr;
    rpc_loc_ioctl_data_u_type           ioctl_data;
    boolean                             ret_val;

    LOGV("loc_eng_process_atl_deferred_action: agps_status = %d", loc_eng_data.agps_status);

    memset (&ioctl_data, 0, sizeof (rpc_loc_ioctl_data_u_type));
 
    if (data_connection_closed)
    {
        ioctl_data.disc = RPC_LOC_IOCTL_INFORM_SERVER_CLOSE_STATUS;
        conn_close_status_ptr = &(ioctl_data.rpc_loc_ioctl_data_u_type_u.conn_close_status);
        conn_close_status_ptr->conn_handle = loc_eng_data.conn_handle;
        conn_close_status_ptr->close_status = RPC_LOC_SERVER_CLOSE_SUCCESS;
    }
    else
    {
        ioctl_data.disc = RPC_LOC_IOCTL_INFORM_SERVER_OPEN_STATUS;
        conn_open_status_ptr = &ioctl_data.rpc_loc_ioctl_data_u_type_u.conn_open_status;
        conn_open_status_ptr->conn_handle = loc_eng_data.conn_handle;
        if (data_connection_succeeded)
        {
            conn_open_status_ptr->open_status = RPC_LOC_SERVER_OPEN_SUCCESS;
            // Both buffer are of the same maximum size, and the source is null terminated
            // strcpy (&(ioctl_data.rpc_loc_ioctl_data_u_type_u.conn_open_status.apn_name), &(loc_eng_data.apn_name));
            conn_open_status_ptr->apn_name = loc_eng_data.apn_name;
            // Delay this so that PDSM ATL module will behave properly
            sleep (1);
            LOGD("loc_eng_process_atl_deferred_action: loc_eng_ioctl for ATL with apn_name = %s", conn_open_status_ptr->apn_name);
        }
        else // data_connection_failed
        {
            conn_open_status_ptr->open_status = RPC_LOC_SERVER_OPEN_FAIL;
        }
        // Delay this so that PDSM ATL module will behave properly
        sleep (1);
    }

    ret_val = loc_eng_ioctl (loc_eng_data.client_handle,
                             ioctl_data.disc,
                             &ioctl_data,
                             LOC_IOCTL_DEFAULT_TIMEOUT,
                             NULL);

    LOGD("loc_eng_process_atl_deferred_action: loc_eng_ioctl for ATL returned %d (1 for success)", ret_val);
}

/*===========================================================================
FUNCTION    loc_eng_process_loc_event

DESCRIPTION
   This is used to process events received from the location engine.

DEPENDENCIES
   None

RETURN VALUE
   N/A

SIDE EFFECTS
   N/A

===========================================================================*/
static void loc_eng_process_loc_event (rpc_loc_event_mask_type loc_event,
        rpc_loc_event_payload_u_type* loc_event_payload)
{
    LOGD("loc_eng_process_loc_event: loc_event = 0x%llx", loc_event);

    if (loc_event & RPC_LOC_EVENT_PARSED_POSITION_REPORT)
    {
        loc_eng_report_position (&(loc_event_payload->rpc_loc_event_payload_u_type_u.parsed_location_report));
    }

    if (loc_event & RPC_LOC_EVENT_SATELLITE_REPORT)
    {
        loc_eng_report_sv (&(loc_event_payload->rpc_loc_event_payload_u_type_u.gnss_report));
    }

    if (loc_event & RPC_LOC_EVENT_STATUS_REPORT)
    {
        loc_eng_report_status (&(loc_event_payload->rpc_loc_event_payload_u_type_u.status_report));
    }

    if (loc_event & RPC_LOC_EVENT_NMEA_POSITION_REPORT)
    {
        loc_eng_report_nmea (&(loc_event_payload->rpc_loc_event_payload_u_type_u.nmea_report));
    }

    // Android XTRA interface supports only XTRA download
    if (loc_event & RPC_LOC_EVENT_ASSISTANCE_DATA_REQUEST)
    {
        if (loc_event_payload->rpc_loc_event_payload_u_type_u.assist_data_request.event ==
                RPC_LOC_ASSIST_DATA_PREDICTED_ORBITS_REQ)
        {
            LOGD("loc_eng_process_loc_event: xtra download requst");

            pthread_mutex_lock(&loc_eng_data.xtra_module_data.xtra_mutex);
            // Call Registered callback
            if (loc_eng_data.xtra_module_data.download_request_cb != NULL) {
                loc_eng_data.xtra_module_data.download_request_cb();
            } else {
                LOGD("loc_eng_process_loc_event: no xtra callback, will download when callback registered");
                loc_eng_data.xtra_module_data.download_request_pending = TRUE;
            }
            pthread_mutex_unlock(&loc_eng_data.xtra_module_data.xtra_mutex);
        }
    }

    if (loc_event & RPC_LOC_EVENT_IOCTL_REPORT)
    {
        // Process the received RPC_LOC_EVENT_IOCTL_REPORT
        (void) loc_eng_ioctl_process_cb (loc_eng_data.client_handle,
                                &(loc_event_payload->rpc_loc_event_payload_u_type_u.ioctl_report));
    }

    if (loc_event & RPC_LOC_EVENT_LOCATION_SERVER_REQUEST)
    {
        loc_eng_process_conn_request (&(loc_event_payload->rpc_loc_event_payload_u_type_u.loc_server_request));
    }

    loc_eng_ni_callback(loc_event, loc_event_payload);

#if DEBUG_MOCK_NI == 1
    // DEBUG only
    if ((loc_event & RPC_LOC_EVENT_STATUS_REPORT) &&
        loc_event_payload->rpc_loc_event_payload_u_type_u.status_report.
        payload.rpc_loc_status_event_payload_u_type_u.engine_state
        == RPC_LOC_ENGINE_STATE_OFF)
    {
        // Mock an NI request
        pthread_t th;
        pthread_create (&th, NULL, mock_ni, (void*) client_handle);
    }
#endif /* DEBUG_MOCK_NI == 1 */
}

/*===========================================================================
FUNCTION loc_eng_process_deferred_action

DESCRIPTION
   Main routine for the thread to execute certain commands
   that are not safe to be done from within an RPC callback.

DEPENDENCIES
   None

RETURN VALUE
   None

SIDE EFFECTS
   WARNING: Not being fast enough here reboots the phone!!!!

===========================================================================*/
static void* loc_eng_process_deferred_action (void* arg)
{

    int last_agps_status;
    work_item *work;

    LOGD("loc_eng_process_deferred_action: started");

    // make sure we do not run in background scheduling group
    set_sched_policy(gettid(), SP_FOREGROUND);

    while (1) {

        if (loc_eng_data.deferred_action_thread_need_exit == TRUE) break;

        // lock the queue
        pthread_mutex_lock(&loc_eng_data.deferred_action_mutex);

        if (loc_eng_data.work_queue) {
            LOGD("loc_eng_process_deferred_action: processing existing work");
        } else {
            LOGD("loc_eng_process_deferred_action: waiting for work");
            pthread_cond_wait(&loc_eng_data.deferred_action_cond,
                              &loc_eng_data.deferred_action_mutex);
            LOGD("loc_eng_process_deferred_action: processing new work");
        }

        // get head of queue
        work = loc_eng_data.work_queue;

        // we can be notified without work (e.g. thread shutdown)
        if (!work) {
            pthread_mutex_unlock(&loc_eng_data.deferred_action_mutex);
            continue;
        }

        // remove first item from queue
        loc_eng_data.work_queue = work->next;
        // unlock the queue
        pthread_mutex_unlock(&loc_eng_data.deferred_action_mutex);

        // save current agps_status
        last_agps_status = loc_eng_data.agps_status;

        if (work->loc_event != 0) {
            //this may set loc_eng_data.agps_status.status
            loc_eng_process_loc_event(work->loc_event, &work->loc_event_payload);
        }

        // dispose of work item
        free(work);

        // callback if status has changed after this event
        if (loc_eng_data.agps_status != 0 &&
            loc_eng_data.agps_status != last_agps_status &&
            loc_eng_data.agps_status_cb) {

            LOGD("loc_eng_process_deferred_action: calling agps_status_cb(0x%x)", loc_eng_data.agps_status);

            AGpsStatus status;
            status.status = loc_eng_data.agps_status;
            status.type = AGPS_TYPE_SUPL;

            loc_eng_data.agps_status_cb(&status);
        }

    }

    LOGD("loc_eng_process_deferred_action: thread exiting");
    return NULL;
}
