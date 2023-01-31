#!/usr/bin/python
#
# RADIUS tests
# Copyright (c) 2013-2014, Jouni Malinen <j@w1.fi>
#
# This software may be distributed under the terms of the BSD license.
# See README for more details.

import logging
logger = logging.getLogger()
import time

import hostapd

def connect(dev, ssid, wait_connect=True):
    dev.connect(ssid, key_mgmt="WPA-EAP", scan_freq="2412",
                eap="PSK", identity="psk.user@example.com",
                password_hex="0123456789abcdef0123456789abcdef",
                wait_connect=wait_connect)

def test_radius_auth_unreachable(dev, apdev):
    """RADIUS Authentication server unreachable"""
    params = hostapd.wpa2_eap_params(ssid="radius-auth")
    params['auth_server_port'] = "18139"
    hostapd.add_ap(apdev[0]['ifname'], params)
    hapd = hostapd.Hostapd(apdev[0]['ifname'])
    connect(dev[0], "radius-auth", wait_connect=False)
    ev = dev[0].wait_event(["CTRL-EVENT-EAP-STARTED"])
    if ev is None:
        raise Exception("Timeout on EAP start")
    logger.info("Checking for RADIUS retries")
    time.sleep(4)
    mib = hapd.get_mib()
    if "radiusAuthClientAccessRequests" not in mib:
        raise Exception("Missing MIB fields")
    if int(mib["radiusAuthClientAccessRetransmissions"]) < 1:
        raise Exception("Missing RADIUS Authentication retransmission")
    if int(mib["radiusAuthClientPendingRequests"]) < 1:
        raise Exception("Missing pending RADIUS Authentication request")

def test_radius_acct_unreachable(dev, apdev):
    """RADIUS Accounting server unreachable"""
    params = hostapd.wpa2_eap_params(ssid="radius-acct")
    params['acct_server_addr'] = "127.0.0.1"
    params['acct_server_port'] = "18139"
    params['acct_server_shared_secret'] = "radius"
    hostapd.add_ap(apdev[0]['ifname'], params)
    hapd = hostapd.Hostapd(apdev[0]['ifname'])
    connect(dev[0], "radius-acct")
    logger.info("Checking for RADIUS retries")
    time.sleep(4)
    mib = hapd.get_mib()
    if "radiusAccClientRetransmissions" not in mib:
        raise Exception("Missing MIB fields")
    if int(mib["radiusAccClientRetransmissions"]) < 2:
        raise Exception("Missing RADIUS Accounting retransmissions")
    if int(mib["radiusAccClientPendingRequests"]) < 2:
        raise Exception("Missing pending RADIUS Accounting requests")

def test_radius_acct(dev, apdev):
    """RADIUS Accounting"""
    as_hapd = hostapd.Hostapd("as")
    as_mib_start = as_hapd.get_mib(param="radius_server")
    params = hostapd.wpa2_eap_params(ssid="radius-acct")
    params['acct_server_addr'] = "127.0.0.1"
    params['acct_server_port'] = "1813"
    params['acct_server_shared_secret'] = "radius"
    hostapd.add_ap(apdev[0]['ifname'], params)
    hapd = hostapd.Hostapd(apdev[0]['ifname'])
    connect(dev[0], "radius-acct")
    logger.info("Checking for RADIUS counters")
    count = 0
    while True:
        mib = hapd.get_mib()
        if int(mib['radiusAccClientResponses']) >= 2:
            break
        time.sleep(0.1)
        count += 1
        if count > 10:
            raise Exception("Did not receive Accounting-Response packets")

    if int(mib['radiusAccClientRetransmissions']) > 0:
        raise Exception("Unexpected Accounting-Request retransmission")

    as_mib_end = as_hapd.get_mib(param="radius_server")

    req_s = int(as_mib_start['radiusAccServTotalRequests'])
    req_e = int(as_mib_end['radiusAccServTotalRequests'])
    if req_e < req_s + 2:
        raise Exception("Unexpected RADIUS server acct MIB value")

    acc_s = int(as_mib_start['radiusAuthServAccessAccepts'])
    acc_e = int(as_mib_end['radiusAuthServAccessAccepts'])
    if acc_e < acc_s + 1:
        raise Exception("Unexpected RADIUS server auth MIB value")

def test_radius_das_disconnect(dev, apdev):
    """RADIUS Dynamic Authorization Extensions - Disconnect"""
    try:
        import pyrad.client
        import pyrad.packet
        import pyrad.dictionary
        import radius_das
    except ImportError:
        return "skip"

    params = hostapd.wpa2_eap_params(ssid="radius-das")
    params['radius_das_port'] = "3799"
    params['radius_das_client'] = "127.0.0.1 secret"
    params['radius_das_require_event_timestamp'] = "1"
    hapd = hostapd.add_ap(apdev[0]['ifname'], params)
    connect(dev[0], "radius-das")
    addr = dev[0].p2p_interface_addr()
    sta = hapd.get_sta(addr)
    id = sta['dot1xAuthSessionId']

    dict = pyrad.dictionary.Dictionary("dictionary.radius")

    srv = pyrad.client.Client(server="127.0.0.1", acctport=3799,
                              secret="secret", dict=dict)
    srv.retries = 1
    srv.timeout = 1

    logger.info("Disconnect-Request with incorrect secret")
    req = radius_das.DisconnectPacket(dict=dict, secret="incorrect",
                                      User_Name="foo",
                                      NAS_Identifier="localhost",
                                      Event_Timestamp=int(time.time()))
    logger.debug(req)
    try:
        reply = srv.SendPacket(req)
        raise Exception("Unexpected response to Disconnect-Request")
    except pyrad.client.Timeout:
        logger.info("Disconnect-Request with incorrect secret properly ignored")

    logger.info("Disconnect-Request without Event-Timestamp")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      User_Name="psk.user@example.com")
    logger.debug(req)
    try:
        reply = srv.SendPacket(req)
        raise Exception("Unexpected response to Disconnect-Request")
    except pyrad.client.Timeout:
        logger.info("Disconnect-Request without Event-Timestamp properly ignored")

    logger.info("Disconnect-Request with non-matching Event-Timestamp")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      User_Name="psk.user@example.com",
                                      Event_Timestamp=123456789)
    logger.debug(req)
    try:
        reply = srv.SendPacket(req)
        raise Exception("Unexpected response to Disconnect-Request")
    except pyrad.client.Timeout:
        logger.info("Disconnect-Request with non-matching Event-Timestamp properly ignored")

    logger.info("Disconnect-Request with unsupported attribute")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      User_Name="foo",
                                      User_Password="foo",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectNAK:
        raise Exception("Unexpected response code")
    if 'Error-Cause' not in reply:
        raise Exception("Missing Error-Cause")
    if reply['Error-Cause'][0] != 401:
        raise Exception("Unexpected Error-Cause: {}".format(reply['Error-Cause']))

    logger.info("Disconnect-Request with invalid Calling-Station-Id")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      User_Name="foo",
                                      Calling_Station_Id="foo",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectNAK:
        raise Exception("Unexpected response code")
    if 'Error-Cause' not in reply:
        raise Exception("Missing Error-Cause")
    if reply['Error-Cause'][0] != 407:
        raise Exception("Unexpected Error-Cause: {}".format(reply['Error-Cause']))

    logger.info("Disconnect-Request with mismatching User-Name")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      User_Name="foo",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectNAK:
        raise Exception("Unexpected response code")
    if 'Error-Cause' not in reply:
        raise Exception("Missing Error-Cause")
    if reply['Error-Cause'][0] != 503:
        raise Exception("Unexpected Error-Cause: {}".format(reply['Error-Cause']))

    logger.info("Disconnect-Request with mismatching Calling-Station-Id")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      Calling_Station_Id="12:34:56:78:90:aa",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectNAK:
        raise Exception("Unexpected response code")
    if 'Error-Cause' not in reply:
        raise Exception("Missing Error-Cause")
    if reply['Error-Cause'][0] != 503:
        raise Exception("Unexpected Error-Cause: {}".format(reply['Error-Cause']))

    logger.info("Disconnect-Request with mismatching Acct-Session-Id")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      Acct_Session_Id="12345678-87654321",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectNAK:
        raise Exception("Unexpected response code")
    if 'Error-Cause' not in reply:
        raise Exception("Missing Error-Cause")
    if reply['Error-Cause'][0] != 503:
        raise Exception("Unexpected Error-Cause: {}".format(reply['Error-Cause']))

    ev = dev[0].wait_event(["CTRL-EVENT-DISCONNECTED"], timeout=1)
    if ev is not None:
        raise Exception("Unexpected disconnection")

    logger.info("Disconnect-Request with matching Acct-Session-Id")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      Acct_Session_Id=id,
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectACK:
        raise Exception("Unexpected response code")

    ev = dev[0].wait_event(["CTRL-EVENT-DISCONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for disconnection")
    ev = dev[0].wait_event(["CTRL-EVENT-CONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for re-connection")

    logger.info("Disconnect-Request with matching User-Name")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      User_Name="psk.user@example.com",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectACK:
        raise Exception("Unexpected response code")

    ev = dev[0].wait_event(["CTRL-EVENT-DISCONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for disconnection")
    ev = dev[0].wait_event(["CTRL-EVENT-CONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for re-connection")

    logger.info("Disconnect-Request with matching Calling-Station-Id")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      Calling_Station_Id=addr,
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectACK:
        raise Exception("Unexpected response code")

    ev = dev[0].wait_event(["CTRL-EVENT-DISCONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for disconnection")
    ev = dev[0].wait_event(["CTRL-EVENT-CONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for re-connection")

    logger.info("Disconnect-Request with matching Calling-Station-Id and non-matching CUI")
    req = radius_das.DisconnectPacket(dict=dict, secret="secret",
                                      Calling_Station_Id=addr,
                                      Chargeable_User_Identity="foo@example.com",
                                      Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.DisconnectACK:
        raise Exception("Unexpected response code")

    ev = dev[0].wait_event(["CTRL-EVENT-DISCONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for disconnection")
    ev = dev[0].wait_event(["CTRL-EVENT-CONNECTED"])
    if ev is None:
        raise Exception("Timeout while waiting for re-connection")

def test_radius_das_coa(dev, apdev):
    """RADIUS Dynamic Authorization Extensions - CoA"""
    try:
        import pyrad.client
        import pyrad.packet
        import pyrad.dictionary
        import radius_das
    except ImportError:
        return "skip"

    params = hostapd.wpa2_eap_params(ssid="radius-das")
    params['radius_das_port'] = "3799"
    params['radius_das_client'] = "127.0.0.1 secret"
    params['radius_das_require_event_timestamp'] = "1"
    hapd = hostapd.add_ap(apdev[0]['ifname'], params)
    connect(dev[0], "radius-das")
    addr = dev[0].p2p_interface_addr()
    sta = hapd.get_sta(addr)
    id = sta['dot1xAuthSessionId']

    dict = pyrad.dictionary.Dictionary("dictionary.radius")

    srv = pyrad.client.Client(server="127.0.0.1", acctport=3799,
                              secret="secret", dict=dict)
    srv.retries = 1
    srv.timeout = 1

    # hostapd does not currently support CoA-Request, so NAK is expected
    logger.info("CoA-Request with matching Acct-Session-Id")
    req = radius_das.CoAPacket(dict=dict, secret="secret",
                               Acct_Session_Id=id,
                               Event_Timestamp=int(time.time()))
    reply = srv.SendPacket(req)
    logger.debug("RADIUS response from hostapd")
    for i in reply.keys():
        logger.debug("%s: %s" % (i, reply[i]))
    if reply.code != pyrad.packet.CoANAK:
        raise Exception("Unexpected response code")
    if 'Error-Cause' not in reply:
        raise Exception("Missing Error-Cause")
    if reply['Error-Cause'][0] != 405:
        raise Exception("Unexpected Error-Cause: {}".format(reply['Error-Cause']))
