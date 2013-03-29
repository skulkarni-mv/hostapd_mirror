#!/usr/bin/python
#
# TDLS tests
# Copyright (c) 2013, Jouni Malinen <j@w1.fi>
#
# This software may be distributed under the terms of the BSD license.
# See README for more details.

import time
import subprocess
import logging
logger = logging.getLogger(__name__)

import hwsim_utils
from hostapd import HostapdGlobal
from hostapd import Hostapd

ap_ifname = 'wlan2'

def start_ap_wpa2_psk(ifname):
    logger.info("Starting WPA2-PSK AP " + ifname)
    hapd_global = HostapdGlobal()
    hapd_global.add(ifname)
    hapd = Hostapd(ifname)
    if not hapd.ping():
        raise Exception("Could not ping hostapd")
    hapd.set_wpa2_psk("test-wpa2-psk", "12345678")
    hapd.enable()

def connect_sta(sta):
    logger.info("Connect STA " + sta.ifname + " to AP")
    id = sta.add_network()
    sta.set_network_quoted(id, "ssid", "test-wpa2-psk")
    sta.set_network_quoted(id, "psk", "12345678")
    sta.connect_network(id)

def connect_2sta(dev):
    connect_sta(dev[0])
    connect_sta(dev[1])
    hwsim_utils.test_connectivity_sta(dev[0], dev[1])
    hwsim_utils.test_connectivity(dev[0].ifname, "wlan2")
    hwsim_utils.test_connectivity(dev[1].ifname, "wlan2")

def wlantest_tdls(field, bssid, addr1, addr2):
    res = subprocess.check_output(["../../wlantest/wlantest_cli",
                                   "get_tdls_counter", field, bssid, addr1,
                                   addr2]);
    if "FAIL" in res:
        raise Exception("wlantest_cli command failed")
    return int(res)

def wlantest_tdls_clear(bssid, addr1, addr2):
    subprocess.call(["../../wlantest/wlantest_cli",
                     "clear_tdls_counters", bssid, addr1, addr2]);

def wlantest_setup():
    subprocess.call(["../../wlantest/wlantest_cli", "flush"]);
    subprocess.call(["../../wlantest/wlantest_cli", "add_passphrase",
                     "12345678"]);

def wlantest_tdls_packet_counters(bssid, addr0, addr1):
    dl = wlantest_tdls("valid_direct_link", bssid, addr0, addr1);
    inv_dl = wlantest_tdls("invalid_direct_link", bssid, addr0, addr1);
    ap = wlantest_tdls("valid_ap_path", bssid, addr0, addr1);
    inv_ap = wlantest_tdls("invalid_ap_path", bssid, addr0, addr1);
    return [dl,inv_dl,ap,inv_ap]

def tdls_check_dl(sta0, sta1, bssid, addr0, addr1):
    wlantest_tdls_clear(bssid, addr0, addr1);
    hwsim_utils.test_connectivity_sta(sta0, sta1)
    [dl,inv_dl,ap,inv_ap] = wlantest_tdls_packet_counters(bssid, addr0, addr1)
    if dl == 0:
        raise Exception("No valid frames through direct link")
    if inv_dl > 0:
        raise Exception("Invalid frames through direct link")
    if ap > 0:
        raise Exception("Unexpected frames through AP path")
    if inv_ap > 0:
        raise Exception("Invalid frames through AP path")

def tdls_check_ap(sta0, sta1, bssid, addr0, addr1):
    wlantest_tdls_clear(bssid, addr0, addr1);
    hwsim_utils.test_connectivity_sta(sta0, sta1)
    [dl,inv_dl,ap,inv_ap] = wlantest_tdls_packet_counters(bssid, addr0, addr1)
    if dl > 0:
        raise Exception("Unexpected frames through direct link")
    if inv_dl > 0:
        raise Exception("Invalid frames through direct link")
    if ap == 0:
        raise Exception("No valid frames through AP path")
    if inv_ap > 0:
        raise Exception("Invalid frames through AP path")

def setup_tdls(sta0, sta1, bssid, reverse=False, expect_fail=False):
    logger.info("Setup TDLS")
    addr0 = sta0.p2p_interface_addr()
    addr1 = sta1.p2p_interface_addr()
    wlantest_tdls_clear(bssid, addr0, addr1);
    wlantest_tdls_clear(bssid, addr1, addr0);
    sta0.tdls_setup(addr1)
    time.sleep(1)
    if expect_fail:
        tdls_check_ap(sta0, sta1, bssid, addr0, addr1)
        return
    if reverse:
        addr1 = sta0.p2p_interface_addr()
        addr0 = sta1.p2p_interface_addr()
    conf = wlantest_tdls("setup_conf_ok", bssid, addr0, addr1);
    if conf == 0:
        raise Exception("No TDLS Setup Confirm (success) seen")
    tdls_check_dl(sta0, sta1, bssid, addr0, addr1)

def teardown_tdls(sta0, sta1, bssid):
    logger.info("Teardown TDLS")
    addr0 = sta0.p2p_interface_addr()
    addr1 = sta1.p2p_interface_addr()
    sta0.tdls_teardown(addr1)
    time.sleep(1)
    teardown = wlantest_tdls("teardown", bssid, addr0, addr1);
    if teardown == 0:
        raise Exception("No TDLS Setup Teardown seen")
    tdls_check_ap(sta0, sta1, bssid, addr0, addr1)

def test_ap_wpa2_tdls(dev):
    """WPA2-PSK AP and two stations using TDLS"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    setup_tdls(dev[0], dev[1], bssid)
    teardown_tdls(dev[0], dev[1], bssid)
    setup_tdls(dev[1], dev[0], bssid)
    #teardown_tdls(dev[0], dev[1], bssid)

def test_ap_wpa2_tdls_concurrent_init(dev):
    """Concurrent TDLS setup initiation"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[0].request("SET tdls_testing 0x80")
    setup_tdls(dev[1], dev[0], bssid, reverse=True)

def test_ap_wpa2_tdls_concurrent_init2(dev):
    """Concurrent TDLS setup initiation (reverse)"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[1].request("SET tdls_testing 0x80")
    setup_tdls(dev[0], dev[1], bssid)

def test_ap_wpa2_tdls_decline_resp(dev):
    """Decline TDLS Setup Response"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[1].request("SET tdls_testing 0x200")
    setup_tdls(dev[1], dev[0], bssid, expect_fail=True)

def test_ap_wpa2_tdls_long_lifetime(dev):
    """TDLS with long TPK lifetime"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[1].request("SET tdls_testing 0x40")
    setup_tdls(dev[1], dev[0], bssid)

def test_ap_wpa2_tdls_long_frame(dev):
    """TDLS with long setup/teardown frames"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[0].request("SET tdls_testing 0x1")
    dev[1].request("SET tdls_testing 0x1")
    setup_tdls(dev[1], dev[0], bssid)
    teardown_tdls(dev[1], dev[0], bssid)
    setup_tdls(dev[0], dev[1], bssid)

def test_ap_wpa2_tdls_reneg(dev):
    """Renegotiate TDLS link"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    setup_tdls(dev[1], dev[0], bssid)
    setup_tdls(dev[0], dev[1], bssid)

def test_ap_wpa2_tdls_wrong_lifetime_resp(dev):
    """Incorrect TPK lifetime in TDLS Setup Response"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[1].request("SET tdls_testing 0x10")
    setup_tdls(dev[0], dev[1], bssid, expect_fail=True)

def test_ap_wpa2_tdls_diff_rsnie(dev):
    """TDLS with different RSN IEs"""
    start_ap_wpa2_psk(ap_ifname)
    bssid = "02:00:00:00:02:00"
    wlantest_setup()
    connect_2sta(dev)
    dev[1].request("SET tdls_testing 0x2")
    setup_tdls(dev[1], dev[0], bssid)
    teardown_tdls(dev[1], dev[0], bssid)

def add_tests(tests):
    tests.append(test_ap_wpa2_tdls)
    tests.append(test_ap_wpa2_tdls_concurrent_init)
    tests.append(test_ap_wpa2_tdls_concurrent_init2)
    tests.append(test_ap_wpa2_tdls_decline_resp)
    tests.append(test_ap_wpa2_tdls_long_lifetime)
    tests.append(test_ap_wpa2_tdls_long_frame)
    tests.append(test_ap_wpa2_tdls_reneg)
    tests.append(test_ap_wpa2_tdls_wrong_lifetime_resp)
    tests.append(test_ap_wpa2_tdls_diff_rsnie)
