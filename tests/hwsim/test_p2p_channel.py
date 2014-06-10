# P2P channel selection test cases
# Copyright (c) 2014, Jouni Malinen <j@w1.fi>
#
# This software may be distributed under the terms of the BSD license.
# See README for more details.

import logging
logger = logging.getLogger()
import os
import subprocess
import time

import hostapd
import hwsim_utils
from test_p2p_grpform import go_neg_pin_authorized
from test_p2p_grpform import check_grpform_results
from test_p2p_grpform import remove_group
from test_p2p_grpform import go_neg_pbc
from test_p2p_autogo import autogo

def set_country(country):
    subprocess.call(['sudo', 'iw', 'reg', 'set', country])
    time.sleep(0.1)

def test_p2p_channel_5ghz(dev):
    """P2P group formation with 5 GHz preference"""
    try:
        set_country("US")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq < 5000:
            raise Exception("Unexpected channel %d MHz - did not follow 5 GHz preference" % freq)
        remove_group(dev[0], dev[1])
    finally:
        set_country("00")

def test_p2p_channel_5ghz_no_vht(dev):
    """P2P group formation with 5 GHz preference when VHT channels are disallowed"""
    try:
        set_country("US")
        dev[0].request("P2P_SET disallow_freq 5180-5240")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq < 5000:
            raise Exception("Unexpected channel %d MHz - did not follow 5 GHz preference" % freq)
        remove_group(dev[0], dev[1])
    finally:
        set_country("00")
        dev[0].request("P2P_SET disallow_freq ")

def test_p2p_channel_random_social(dev):
    """P2P group formation with 5 GHz preference but all 5 GHz channels disabled"""
    try:
        set_country("US")
        dev[0].request("SET p2p_oper_channel 11")
        dev[0].request("P2P_SET disallow_freq 5000-6000,2462")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq not in [ 2412, 2437, 2462 ]:
            raise Exception("Unexpected channel %d MHz - did not pick random social channel" % freq)
        remove_group(dev[0], dev[1])
    finally:
        set_country("00")
        dev[0].request("P2P_SET disallow_freq ")

def test_p2p_channel_random(dev):
    """P2P group formation with 5 GHz preference but all 5 GHz channels and all social channels disabled"""
    try:
        set_country("US")
        dev[0].request("SET p2p_oper_channel 11")
        dev[0].request("P2P_SET disallow_freq 5000-6000,2412,2437,2462")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq > 2500 or freq in [ 2412, 2437, 2462 ]:
            raise Exception("Unexpected channel %d MHz" % freq)
        remove_group(dev[0], dev[1])
    finally:
        set_country("00")
        dev[0].request("P2P_SET disallow_freq ")

def test_p2p_channel_random_social_with_op_class_change(dev, apdev, params):
    """P2P group formation using random social channel with oper class change needed"""
    try:
        set_country("US")
        logger.info("Start group on 5 GHz")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq < 5000:
            raise Exception("Unexpected channel %d MHz - did not pick 5 GHz preference" % freq)
        remove_group(dev[0], dev[1])

        logger.info("Disable 5 GHz and try to re-start group based on 5 GHz preference")
        dev[0].request("SET p2p_oper_reg_class 115")
        dev[0].request("SET p2p_oper_channel 36")
        dev[0].request("P2P_SET disallow_freq 5000-6000")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq not in [ 2412, 2437, 2462 ]:
            raise Exception("Unexpected channel %d MHz - did not pick random social channel" % freq)
        remove_group(dev[0], dev[1])

        try:
            arg = [ "tshark",
                    "-r", os.path.join(params['logdir'], "hwsim0.pcapng"),
                    "-R", "wifi_p2p.public_action.subtype == 0",
                    "-V" ]
            cmd = subprocess.Popen(arg, stdout=subprocess.PIPE,
                                   stderr=open('/dev/null', 'w'))
        except Exception, e:
            logger.info("Could run run tshark check: " + str(e))
            cmd = None
            pass

        if cmd:
            last = None
            for l in cmd.stdout.read().splitlines():
                if "Operating Channel:" not in l:
                    continue
                last = l
            if last is None:
                raise Exception("Could not find GO Negotiation Request")
            if "Operating Class 81" not in last:
                raise Exception("Unexpected operating class: " + last.strip())
    finally:
        set_country("00")
        dev[0].request("P2P_SET disallow_freq ")
        dev[0].request("SET p2p_oper_reg_class 81")
        dev[0].request("SET p2p_oper_channel 11")

def test_p2p_channel_avoid(dev):
    """P2P and avoid frequencies driver event"""
    try:
        set_country("US")
        if "OK" not in dev[0].request("DRIVER_EVENT AVOID_FREQUENCIES 5000-6000,2412,2437,2462"):
            raise Exception("Could not simulate driver event")
        ev = dev[0].wait_event(["CTRL-EVENT-AVOID-FREQ"], timeout=10)
        if ev is None:
            raise Exception("No CTRL-EVENT-AVOID-FREQ event")
        [i_res, r_res] = go_neg_pin_authorized(i_dev=dev[0], i_intent=15,
                                               r_dev=dev[1], r_intent=0,
                                               test_data=False)
        check_grpform_results(i_res, r_res)
        freq = int(i_res['freq'])
        if freq > 2500 or freq in [ 2412, 2437, 2462 ]:
            raise Exception("Unexpected channel %d MHz" % freq)

        if "OK" not in dev[0].request("DRIVER_EVENT AVOID_FREQUENCIES"):
            raise Exception("Could not simulate driver event(2)")
        ev = dev[0].wait_event(["CTRL-EVENT-AVOID-FREQ"], timeout=10)
        if ev is None:
            raise Exception("No CTRL-EVENT-AVOID-FREQ event")
        ev = dev[0].wait_event(["P2P-REMOVE-AND-REFORM-GROUP"], timeout=1)
        if ev is not None:
            raise Exception("Unexpected P2P-REMOVE-AND-REFORM-GROUP event")

        if "OK" not in dev[0].request("DRIVER_EVENT AVOID_FREQUENCIES " + str(freq)):
            raise Exception("Could not simulate driver event(3)")
        ev = dev[0].wait_event(["CTRL-EVENT-AVOID-FREQ"], timeout=10)
        if ev is None:
            raise Exception("No CTRL-EVENT-AVOID-FREQ event")
        ev = dev[0].wait_event(["P2P-REMOVE-AND-REFORM-GROUP"], timeout=10)
        if ev is None:
            raise Exception("No P2P-REMOVE-AND-REFORM-GROUP event")
    finally:
        set_country("00")
        dev[0].request("DRIVER_EVENT AVOID_FREQUENCIES")

def test_autogo_following_bss(dev, apdev):
    """P2P autonomous GO operate on the same channel as station interface"""
    if dev[0].get_mcc() > 1:
        logger.info("test mode: MCC")

    dev[0].request("SET p2p_no_group_iface 0")

    channels = { 3 : "2422", 5 : "2432", 9 : "2452" }
    for key in channels:
        hostapd.add_ap(apdev[0]['ifname'], { "ssid" : 'ap-test',
                                             "channel" : str(key) })
        dev[0].connect("ap-test", key_mgmt="NONE",
                       scan_freq=str(channels[key]))
        res_go = autogo(dev[0])
        if res_go['freq'] != channels[key]:
            raise Exception("Group operation channel is not the same as on connected station interface")
        hwsim_utils.test_connectivity(dev[0].ifname, apdev[0]['ifname'])
        dev[0].remove_group(res_go['ifname'])

def test_go_neg_with_bss_connected(dev, apdev):
    """P2P channel selection: GO negotiation when station interface is connected"""

    dev[0].request("SET p2p_no_group_iface 0")

    hostapd.add_ap(apdev[0]['ifname'], { "ssid": 'bss-2.4ghz', "channel": '5' })
    dev[0].connect("bss-2.4ghz", key_mgmt="NONE", scan_freq="2432")
    #dev[0] as GO
    [i_res, r_res] = go_neg_pbc(i_dev=dev[0], i_intent=10, r_dev=dev[1],
                                r_intent=1)
    check_grpform_results(i_res, r_res)
    if i_res['role'] != "GO":
       raise Exception("GO not selected according to go_intent")
    if i_res['freq'] != "2432":
       raise Exception("Group formed on a different frequency than BSS")
    hwsim_utils.test_connectivity(dev[0].ifname, apdev[0]['ifname'])
    dev[0].remove_group(i_res['ifname'])

    if dev[0].get_mcc() > 1:
        logger.info("Skip as-client case due to MCC being enabled")
        return;

    #dev[0] as client
    [i_res2, r_res2] = go_neg_pbc(i_dev=dev[0], i_intent=1, r_dev=dev[1],
                                  r_intent=10)
    check_grpform_results(i_res2, r_res2)
    if i_res2['role'] != "client":
       raise Exception("GO not selected according to go_intent")
    if i_res2['freq'] != "2432":
       raise Exception("Group formed on a different frequency than BSS")
    hwsim_utils.test_connectivity(dev[0].ifname, apdev[0]['ifname'])

def test_autogo_with_bss_on_disallowed_chan(dev, apdev):
    """P2P channel selection: Autonomous GO with BSS on a disallowed channel"""

    dev[0].request("SET p2p_no_group_iface 0")

    if dev[0].get_mcc() < 2:
       logger.info("Skipping test because driver does not support MCC")
       return "skip"
    try:
        hostapd.add_ap(apdev[0]['ifname'], { "ssid": 'bss-2.4ghz',
                                             "channel": '1' })
        dev[0].request("P2P_SET disallow_freq 2412")
        dev[0].connect("bss-2.4ghz", key_mgmt="NONE", scan_freq="2412")
        res = autogo(dev[0])
        if res['freq'] == "2412":
           raise Exception("GO set on a disallowed channel")
        hwsim_utils.test_connectivity(dev[0].ifname, apdev[0]['ifname'])
    finally:
        dev[0].request("P2P_SET disallow_freq ")
