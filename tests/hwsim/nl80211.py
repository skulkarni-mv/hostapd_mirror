# nl80211 definitions
# Copyright (c) 2014, Jouni Malinen <j@w1.fi>
#
# This software may be distributed under the terms of the BSD license.
# See README for more details.

import binascii
import struct

nl80211_cmd = {
    'GET_WIPHY': 1,
    'SET_WIPHY': 2,
    'NEW_WIPHY': 3,
    'DEL_WIPHY': 4,
    'GET_INTERFACE': 5,
    'SET_INTERFACE': 6,
    'NEW_INTERFACE': 7,
    'DEL_INTERFACE': 8,
    'GET_KEY': 9,
    'SET_KEY': 10,
    'NEW_KEY': 11,
    'DEL_KEY': 12,
    'GET_BEACON': 13,
    'SET_BEACON': 14,
    'START_AP': 15,
    'STOP_AP': 16,
    'GET_STATION': 17,
    'SET_STATION': 18,
    'NEW_STATION': 19,
    'DEL_STATION': 20,
    'GET_MPATH': 21,
    'SET_MPATH': 22,
    'NEW_MPATH': 23,
    'DEL_MPATH': 24,
    'SET_BSS': 25,
    'SET_REG': 26,
    'REQ_SET_REG': 27,
    'GET_MESH_CONFIG': 28,
    'SET_MESH_CONFIG': 29,
    'SET_MGMT_EXTRA_IE[RESERVED]': 30,
    'GET_REG': 31,
    'GET_SCAN': 32,
    'TRIGGER_SCAN': 33,
    'NEW_SCAN_RESULTS': 34,
    'SCAN_ABORTED': 35,
    'REG_CHANGE': 36,
    'AUTHENTICATE': 37,
    'ASSOCIATE': 38,
    'DEAUTHENTICATE': 39,
    'DISASSOCIATE': 40,
    'MICHAEL_MIC_FAILURE': 41,
    'REG_BEACON_HINT': 42,
    'JOIN_IBSS': 43,
    'LEAVE_IBSS': 44,
    'TESTMODE': 45,
    'CONNECT': 46,
    'ROAM': 47,
    'DISCONNECT': 48,
    'SET_WIPHY_NETNS': 49,
    'GET_SURVEY': 50,
    'NEW_SURVEY_RESULTS': 51,
    'SET_PMKSA': 52,
    'DEL_PMKSA': 53,
    'FLUSH_PMKSA': 54,
    'REMAIN_ON_CHANNEL': 55,
    'CANCEL_REMAIN_ON_CHANNEL': 56,
    'SET_TX_BITRATE_MASK': 57,
    'REGISTER_FRAME': 58,
    'FRAME': 59,
    'FRAME_TX_STATUS': 60,
    'SET_POWER_SAVE': 61,
    'GET_POWER_SAVE': 62,
    'SET_CQM': 63,
    'NOTIFY_CQM': 64,
    'SET_CHANNEL': 65,
    'SET_WDS_PEER': 66,
    'FRAME_WAIT_CANCEL': 67,
    'JOIN_MESH': 68,
    'LEAVE_MESH': 69,
    'UNPROT_DEAUTHENTICATE': 70,
    'UNPROT_DISASSOCIATE': 71,
    'NEW_PEER_CANDIDATE': 72,
    'GET_WOWLAN': 73,
    'SET_WOWLAN': 74,
    'START_SCHED_SCAN': 75,
    'STOP_SCHED_SCAN': 76,
    'SCHED_SCAN_RESULTS': 77,
    'SCHED_SCAN_STOPPED': 78,
    'SET_REKEY_OFFLOAD': 79,
    'PMKSA_CANDIDATE': 80,
    'TDLS_OPER': 81,
    'TDLS_MGMT': 82,
    'UNEXPECTED_FRAME': 83,
    'PROBE_CLIENT': 84,
    'REGISTER_BEACONS': 85,
    'UNEXPECTED_4ADDR_FRAME': 86,
    'SET_NOACK_MAP': 87,
    'CH_SWITCH_NOTIFY': 88,
    'START_P2P_DEVICE': 89,
    'STOP_P2P_DEVICE': 90,
    'CONN_FAILED': 91,
    'SET_MCAST_RATE': 92,
    'SET_MAC_ACL': 93,
    'RADAR_DETECT': 94,
    'GET_PROTOCOL_FEATURES': 95,
    'UPDATE_FT_IES': 96,
    'FT_EVENT': 97,
    'CRIT_PROTOCOL_START': 98,
    'CRIT_PROTOCOL_STOP': 99,
    'GET_COALESCE': 100,
    'SET_COALESCE': 101,
    'CHANNEL_SWITCH': 102,
    'VENDOR': 103,
    'SET_QOS_MAP': 104,
}

nl80211_attr = {
    'WIPHY': 1,
    'WIPHY_NAME': 2,
    'IFINDEX': 3,
    'IFNAME': 4,
    'IFTYPE': 5,
    'MAC': 6,
    'KEY_DATA': 7,
    'KEY_IDX': 8,
    'KEY_CIPHER': 9,
    'KEY_SEQ': 10,
    'KEY_DEFAULT': 11,
    'BEACON_INTERVAL': 12,
    'DTIM_PERIOD': 13,
    'BEACON_HEAD': 14,
    'BEACON_TAIL': 15,
    'STA_AID': 16,
    'STA_FLAGS': 17,
    'STA_LISTEN_INTERVAL': 18,
    'STA_SUPPORTED_RATES': 19,
    'STA_VLAN': 20,
    'STA_INFO': 21,
    'WIPHY_BANDS': 22,
    'MNTR_FLAGS': 23,
    'MESH_ID': 24,
    'STA_PLINK_ACTION': 25,
    'MPATH_NEXT_HOP': 26,
    'MPATH_INFO': 27,
    'BSS_CTS_PROT': 28,
    'BSS_SHORT_PREAMBLE': 29,
    'BSS_SHORT_SLOT_TIME': 30,
    'HT_CAPABILITY': 31,
    'SUPPORTED_IFTYPES': 32,
    'REG_ALPHA2': 33,
    'REG_RULES': 34,
    'MESH_CONFIG': 35,
    'BSS_BASIC_RATES': 36,
    'WIPHY_TXQ_PARAMS': 37,
    'WIPHY_FREQ': 38,
    'WIPHY_CHANNEL_TYPE': 39,
    'KEY_DEFAULT_MGMT': 40,
    'MGMT_SUBTYPE': 41,
    'IE': 42,
    'MAX_NUM_SCAN_SSIDS': 43,
    'SCAN_FREQUENCIES': 44,
    'SCAN_SSIDS': 45,
    'GENERATION': 46,
    'BSS': 47,
    'REG_INITIATOR': 48,
    'REG_TYPE': 49,
    'SUPPORTED_COMMANDS': 50,
    'FRAME': 51,
    'SSID': 52,
    'AUTH_TYPE': 53,
    'REASON_CODE': 54,
    'KEY_TYPE': 55,
    'MAX_SCAN_IE_LEN': 56,
    'CIPHER_SUITES': 57,
    'FREQ_BEFORE': 58,
    'FREQ_AFTER': 59,
    'FREQ_FIXED': 60,
    'WIPHY_RETRY_SHORT': 61,
    'WIPHY_RETRY_LONG': 62,
    'WIPHY_FRAG_THRESHOLD': 63,
    'WIPHY_RTS_THRESHOLD': 64,
    'TIMED_OUT': 65,
    'USE_MFP': 66,
    'STA_FLAGS2': 67,
    'CONTROL_PORT': 68,
    'TESTDATA': 69,
    'PRIVACY': 70,
    'DISCONNECTED_BY_AP': 71,
    'STATUS_CODE': 72,
    'CIPHER_SUITES_PAIRWISE': 73,
    'CIPHER_SUITE_GROUP': 74,
    'WPA_VERSIONS': 75,
    'AKM_SUITES': 76,
    'REQ_IE': 77,
    'RESP_IE': 78,
    'PREV_BSSID': 79,
    'KEY': 80,
    'KEYS': 81,
    'PID': 82,
    '4ADDR': 83,
    'SURVEY_INFO': 84,
    'PMKID': 85,
    'MAX_NUM_PMKIDS': 86,
    'DURATION': 87,
    'COOKIE': 88,
    'WIPHY_COVERAGE_CLASS': 89,
    'TX_RATES': 90,
    'FRAME_MATCH': 91,
    'ACK': 92,
    'PS_STATE': 93,
    'CQM': 94,
    'LOCAL_STATE_CHANGE': 95,
    'AP_ISOLATE': 96,
    'WIPHY_TX_POWER_SETTING': 97,
    'WIPHY_TX_POWER_LEVEL': 98,
    'TX_FRAME_TYPES': 99,
    'RX_FRAME_TYPES': 100,
    'FRAME_TYPE': 101,
    'CONTROL_PORT_ETHERTYPE': 102,
    'CONTROL_PORT_NO_ENCRYPT': 103,
    'SUPPORT_IBSS_RSN': 104,
    'WIPHY_ANTENNA_TX': 105,
    'WIPHY_ANTENNA_RX': 106,
    'MCAST_RATE': 107,
    'OFFCHANNEL_TX_OK': 108,
    'BSS_HT_OPMODE': 109,
    'KEY_DEFAULT_TYPES': 110,
    'MAX_REMAIN_ON_CHANNEL_DURATION': 111,
    'MESH_SETUP': 112,
    'WIPHY_ANTENNA_AVAIL_TX': 113,
    'WIPHY_ANTENNA_AVAIL_RX': 114,
    'SUPPORT_MESH_AUTH': 115,
    'STA_PLINK_STATE': 116,
    'WOWLAN_TRIGGERS': 117,
    'WOWLAN_TRIGGERS_SUPPORTED': 118,
    'SCHED_SCAN_INTERVAL': 119,
    'INTERFACE_COMBINATIONS': 120,
    'SOFTWARE_IFTYPES': 121,
    'REKEY_DATA': 122,
    'MAX_NUM_SCHED_SCAN_SSIDS': 123,
    'MAX_SCHED_SCAN_IE_LEN': 124,
    'SCAN_SUPP_RATES': 125,
    'HIDDEN_SSID': 126,
    'IE_PROBE_RESP': 127,
    'IE_ASSOC_RESP': 128,
    'STA_WME': 129,
    'SUPPORT_AP_UAPSD': 130,
    'ROAM_SUPPORT': 131,
    'SCHED_SCAN_MATCH': 132,
    'MAX_MATCH_SETS': 133,
    'PMKSA_CANDIDATE': 134,
    'TX_NO_CCK_RATE': 135,
    'TDLS_ACTION': 136,
    'TDLS_DIALOG_TOKEN': 137,
    'TDLS_OPERATION': 138,
    'TDLS_SUPPORT': 139,
    'TDLS_EXTERNAL_SETUP': 140,
    'DEVICE_AP_SME': 141,
    'DONT_WAIT_FOR_ACK': 142,
    'FEATURE_FLAGS': 143,
    'PROBE_RESP_OFFLOAD': 144,
    'PROBE_RESP': 145,
    'DFS_REGION': 146,
    'DISABLE_HT': 147,
    'HT_CAPABILITY_MASK': 148,
    'NOACK_MAP': 149,
    'INACTIVITY_TIMEOUT': 150,
    'RX_SIGNAL_DBM': 151,
    'BG_SCAN_PERIOD': 152,
    'WDEV': 153,
    'USER_REG_HINT_TYPE': 154,
    'CONN_FAILED_REASON': 155,
    'SAE_DATA': 156,
    'VHT_CAPABILITY': 157,
    'SCAN_FLAGS': 158,
    'CHANNEL_WIDTH': 159,
    'CENTER_FREQ1': 160,
    'CENTER_FREQ2': 161,
    'P2P_CTWINDOW': 162,
    'P2P_OPPPS': 163,
    'LOCAL_MESH_POWER_MODE': 164,
    'ACL_POLICY': 165,
    'MAC_ADDRS': 166,
    'MAC_ACL_MAX': 167,
    'RADAR_EVENT': 168,
    'EXT_CAPA': 169,
    'EXT_CAPA_MASK': 170,
    'STA_CAPABILITY': 171,
    'STA_EXT_CAPABILITY': 172,
    'PROTOCOL_FEATURES': 173,
    'SPLIT_WIPHY_DUMP': 174,
    'DISABLE_VHT': 175,
    'VHT_CAPABILITY_MASK': 176,
    'MDID': 177,
    'IE_RIC': 178,
    'CRIT_PROT_ID': 179,
    'MAX_CRIT_PROT_DURATION': 180,
    'PEER_AID': 181,
    'COALESCE_RULE': 182,
    'CH_SWITCH_COUNT': 183,
    'CH_SWITCH_BLOCK_TX': 184,
    'CSA_IES': 185,
    'CSA_C_OFF_BEACON': 186,
    'CSA_C_OFF_PRESP': 187,
    'RXMGMT_FLAGS': 188,
    'STA_SUPPORTED_CHANNELS': 189,
    'STA_SUPPORTED_OPER_CLASSES': 190,
    'HANDLE_DFS': 191,
    'SUPPORT_5_MHZ': 192,
    'SUPPORT_10_MHZ': 193,
    'OPMODE_NOTIF': 194,
    'VENDOR_ID': 195,
    'VENDOR_SUBCMD': 196,
    'VENDOR_DATA': 197,
    'VENDOR_EVENTS': 198,
    'QOS_MAP': 199,
    'MAC_HINT': 200,
    'WIPHY_FREQ_HINT': 201,
    'MAX_AP_ASSOC_STA': 202,
}

def build_nl80211_attr(id, val):
    attr = struct.pack("@HH", 4 + len(val), nl80211_attr[id]) + val
    if len(attr) % 4 != 0:
        attr += b'\x00' * (4 - (len(attr) % 4))
    return attr

def build_nl80211_attr_u32(id, val):
    return build_nl80211_attr(id, struct.pack("@I", val))

def build_nl80211_attr_u16(id, val):
    return build_nl80211_attr(id, struct.pack("@H", val))

def build_nl80211_attr_u8(id, val):
    return build_nl80211_attr(id, struct.pack("@B", val))

def build_nl80211_attr_flag(id):
    return build_nl80211_attr(id, b'')

def build_nl80211_attr_mac(id, val):
    addr = struct.unpack('6B', binascii.unhexlify(val.replace(':','')))
    aval = struct.pack('<6B', *addr)
    return build_nl80211_attr(id, aval)

def parse_nl80211_attrs(msg):
    attrs = {}
    while len(msg) >= 4:
        alen,attr = struct.unpack("@HH", msg[0:4])
        if alen < 4:
            raise Exception("Too short nl80211 attribute")
        alen -= 4
        msg = msg[4:]
        if alen > len(msg):
            raise Exception("nl80211 attribute underflow")
        attrs[attr] = msg[0:alen]
        msg = msg[alen:]
    return attrs
