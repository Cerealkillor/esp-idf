/*
 * SPDX-FileCopyrightText: 2019-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "rsn_supp/wpa.h"
#include "rsn_supp/wpa_i.h"
#include "common/eapol_common.h"
#include "common/ieee802_11_defs.h"
#include "rsn_supp/wpa_ie.h"
#include "ap/wpa_auth.h"
#include "ap/wpa_auth_i.h"
#include "ap/ap_config.h"
#include "ap/hostapd.h"
#include "esp_wpas_glue.h"
#include "esp_hostap.h"

#include "esp_system.h"
#include "crypto/crypto.h"
#include "crypto/sha1.h"
#include "crypto/aes_wrap.h"

#include "esp_wifi_driver.h"
#include "esp_private/wifi.h"
#include "esp_wpa3_i.h"
#include "esp_wpa2.h"
#include "esp_common_i.h"

void  wpa_install_key(enum wpa_alg alg, u8 *addr, int key_idx, int set_tx,
                      u8 *seq, size_t seq_len, u8 *key, size_t key_len, int key_entry_valid)
{
    esp_wifi_set_sta_key_internal(alg, addr, key_idx, set_tx, seq, seq_len, key, key_len, key_entry_valid);
}

int  wpa_get_key(uint8_t *ifx, int *alg, u8 *addr, int *key_idx,
                 u8 *key, size_t key_len, int key_entry_valid)
{
    return esp_wifi_get_sta_key_internal(ifx, alg, addr, key_idx, key, key_len, key_entry_valid);
}

/**
 * eapol_sm_notify_eap_success - Notification of external EAP success trigger
 * @sm: Pointer to EAPOL state machine allocated with eapol_sm_init()
 * @success: %TRUE = set success, %FALSE = clear success
 *
 * Notify the EAPOL state machine that external event has forced EAP state to
 * success (success = %TRUE). This can be cleared by setting success = %FALSE.
 *
 * This function is called to update EAP state when WPA-PSK key handshake has
 * been completed successfully since WPA-PSK does not use EAP state machine.
 */

/* fix buf for tx for now */
#define WPA_TX_MSG_BUFF_MAXLEN 200

void  wpa_sendto_wrapper(void *buffer, u16 len)
{
    esp_wifi_internal_tx(0, buffer, len);
}

void  wpa_deauthenticate(u8 reason_code)
{
    esp_wifi_deauthenticate_internal(reason_code);
}

int  wpa_config_profile(void)
{
    if (esp_wifi_sta_prof_is_wpa_internal()) {
        wpa_set_profile(WPA_PROTO_WPA, esp_wifi_sta_get_prof_authmode_internal());
    } else if (esp_wifi_sta_prof_is_wpa2_internal() || esp_wifi_sta_prof_is_wpa3_internal()) {
        wpa_set_profile(WPA_PROTO_RSN, esp_wifi_sta_get_prof_authmode_internal());
    } else if (esp_wifi_sta_prof_is_wapi_internal()) {
        wpa_set_profile(WPA_PROTO_WAPI, esp_wifi_sta_get_prof_authmode_internal());
    } else {
        /* do nothing */
        return -1;
    }
    return 0;
}

int wpa_config_bss(uint8_t *bssid)
{
    int ret = 0;
    struct wifi_ssid *ssid = esp_wifi_sta_get_prof_ssid_internal();
    u8 mac[6];

    esp_wifi_get_macaddr_internal(0, mac);
    ret = wpa_set_bss((char *)mac, (char *)bssid, esp_wifi_sta_get_pairwise_cipher_internal(), esp_wifi_sta_get_group_cipher_internal(),
                (char *)esp_wifi_sta_get_prof_password_internal(), ssid->ssid, ssid->len);
    return ret;
}

void  wpa_config_assoc_ie(u8 proto, u8 *assoc_buf, u32 assoc_wpa_ie_len)
{
    if (proto == BIT(0)) {
        esp_wifi_set_appie_internal(WIFI_APPIE_WPA, assoc_buf, assoc_wpa_ie_len, 1);
    } else {
        esp_wifi_set_appie_internal(WIFI_APPIE_RSN, assoc_buf, assoc_wpa_ie_len, 1);
    }
}

void  wpa_neg_complete(void)
{
    esp_wifi_auth_done_internal();
}

bool  wpa_attach(void)
{
    bool ret = true;
    ret = wpa_sm_init(NULL, wpa_sendto_wrapper,
                 wpa_config_assoc_ie, wpa_install_key, wpa_get_key, wpa_deauthenticate, wpa_neg_complete);
    if(ret) {
        ret = (esp_wifi_register_tx_cb_internal(eapol_txcb, WIFI_TXCB_EAPOL_ID) == ESP_OK);
    }
    esp_set_scan_ie();
    esp_set_assoc_ie();
    return ret;
}

uint8_t  *wpa_ap_get_wpa_ie(uint8_t *ie_len)
{
    struct hostapd_data *hapd = (struct hostapd_data *)esp_wifi_get_hostap_private_internal();

    if (!hapd || !hapd->wpa_auth || !hapd->wpa_auth->wpa_ie) {
        return NULL;
    }

    *ie_len = hapd->wpa_auth->wpa_ie_len;
    return hapd->wpa_auth->wpa_ie;
}

bool  wpa_ap_rx_eapol(void *hapd_data, void *sm_data, u8 *data, size_t data_len)
{
    struct hostapd_data *hapd = (struct hostapd_data *)hapd_data;
    struct wpa_state_machine *sm = (struct wpa_state_machine *)sm_data;

    if (!hapd || !sm) {
        return false;
    }

    wpa_receive(hapd->wpa_auth, sm, data, data_len);

    return true;
}

void wpa_ap_get_peer_spp_msg(void *sm_data, bool *spp_cap, bool *spp_req)
{
    struct wpa_state_machine *sm = (struct wpa_state_machine *)sm_data;

    if (!sm) {
        return;
    }

    *spp_cap = sm->spp_sup.capable;
    *spp_req = sm->spp_sup.require;
}

bool  wpa_deattach(void)
{
    esp_wifi_sta_wpa2_ent_disable();
    wpa_sm_deinit();
    return true;
}

int wpa_sta_connect(uint8_t *bssid)
{
    /* use this API to set AP specific IEs during connection */
    int ret = 0;
    ret = wpa_config_profile();
    if (ret == 0) {
        ret = wpa_config_bss(bssid);
        if (ret) {
            wpa_printf(MSG_DEBUG, "Rejecting bss, validation failed");
            return ret;
        }
    }

    return 0;
}

void wpa_config_done(void)
{
    /* used in future for setting scan and assoc IEs */
    esp_set_assoc_ie();
}

int wpa_parse_wpa_ie_wrapper(const u8 *wpa_ie, size_t wpa_ie_len, wifi_wpa_ie_t *data)
{
    struct wpa_ie_data ie;
    int ret = 0;

    ret = wpa_parse_wpa_ie(wpa_ie, wpa_ie_len, &ie);
    data->proto = ie.proto;
    data->pairwise_cipher = cipher_type_map_supp_to_public(ie.pairwise_cipher);
    data->group_cipher = cipher_type_map_supp_to_public(ie.group_cipher);
    data->key_mgmt = ie.key_mgmt;
    data->capabilities = ie.capabilities;
    data->pmkid = ie.pmkid;
    data->mgmt_group_cipher = cipher_type_map_supp_to_public(ie.mgmt_group_cipher);

    return ret;
}

static void wpa_sta_disconnected_cb(uint8_t reason_code)
{
    switch (reason_code) {
        case WIFI_REASON_UNSPECIFIED:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_NOT_AUTHED:
        case WIFI_REASON_NOT_ASSOCED:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_INVALID_PMKID:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_CONNECTION_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            esp_wpa3_free_sae_data();
            wpa_sta_clear_curr_pmksa();
            break;
        default:
            break;
    }
}

#ifndef ROAMING_SUPPORT
static inline int esp_supplicant_common_init(struct wpa_funcs *wpa_cb)
{
	wpa_cb->wpa_sta_rx_mgmt = NULL;
	wpa_cb->wpa_sta_profile_match = NULL;

	return 0;
}
static inline void esp_supplicant_common_deinit(void)
{
}
#endif

int esp_supplicant_init(void)
{
    int ret = ESP_OK;
    struct wpa_funcs *wpa_cb;

    wpa_cb = (struct wpa_funcs *)os_zalloc(sizeof(struct wpa_funcs));
    if (!wpa_cb) {
        return ESP_ERR_NO_MEM;
    }

    wpa_cb->wpa_sta_init       = wpa_attach;
    wpa_cb->wpa_sta_deinit     = wpa_deattach;
    wpa_cb->wpa_sta_rx_eapol   = wpa_sm_rx_eapol;
    wpa_cb->wpa_sta_connect    = wpa_sta_connect;
    wpa_cb->wpa_sta_disconnected_cb = wpa_sta_disconnected_cb;
    wpa_cb->wpa_sta_in_4way_handshake = wpa_sta_in_4way_handshake;

#ifdef CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    wpa_cb->wpa_ap_join       = wpa_ap_join;
    wpa_cb->wpa_ap_remove     = wpa_ap_remove;
    wpa_cb->wpa_ap_get_wpa_ie = wpa_ap_get_wpa_ie;
    wpa_cb->wpa_ap_rx_eapol   = wpa_ap_rx_eapol;
    wpa_cb->wpa_ap_get_peer_spp_msg  = wpa_ap_get_peer_spp_msg;
    wpa_cb->wpa_ap_init       = hostap_init;
    wpa_cb->wpa_ap_deinit     = hostap_deinit;
#endif

    wpa_cb->wpa_config_parse_string  = wpa_config_parse_string;
    wpa_cb->wpa_parse_wpa_ie  = wpa_parse_wpa_ie_wrapper;
    wpa_cb->wpa_config_bss = NULL;//wpa_config_bss;
    wpa_cb->wpa_michael_mic_failure = wpa_michael_mic_failure;
    wpa_cb->wpa_config_done = wpa_config_done;

    esp_wifi_register_wpa3_cb(wpa_cb);
    ret = esp_supplicant_common_init(wpa_cb);

    if (ret != 0) {
        return ret;
    }

    esp_wifi_register_wpa_cb_internal(wpa_cb);

#if CONFIG_WPA_WAPI_PSK
    ret =  esp_wifi_internal_wapi_init();
#endif

    return ret;
}

int esp_supplicant_deinit(void)
{
    esp_supplicant_common_deinit();
    return esp_wifi_unregister_wpa_cb_internal();
}
