#!/usr/bin/env python2
#
# Parallel VM test case executor
# Copyright (c) 2014-2015, Jouni Malinen <j@w1.fi>
#
# This software may be distributed under the terms of the BSD license.
# See README for more details.

import curses
import fcntl
import logging
import os
import subprocess
import sys
import time

logger = logging.getLogger()

def get_failed(vm):
    failed = []
    for i in range(num_servers):
        failed += vm[i]['failed']
    return failed

def vm_read_stdout(vm, i):
    global total_started, total_passed, total_failed, total_skipped
    global rerun_failures

    ready = False
    try:
        out = vm['proc'].stdout.read()
    except:
        return False
    logger.debug("VM[%d] stdout.read[%s]" % (i, out))
    pending = vm['pending'] + out
    lines = []
    while True:
        pos = pending.find('\n')
        if pos < 0:
            break
        line = pending[0:pos].rstrip()
        pending = pending[(pos + 1):]
        logger.debug("VM[%d] stdout full line[%s]" % (i, line))
        if line.startswith("READY"):
            ready = True
        elif line.startswith("PASS"):
            ready = True
            total_passed += 1
        elif line.startswith("FAIL"):
            ready = True
            total_failed += 1
            name = line.split(' ')[1]
            logger.debug("VM[%d] test case failed: %s" % (i, name))
            vm['failed'].append(name)
        elif line.startswith("NOT-FOUND"):
            ready = True
            total_failed += 1
            logger.info("VM[%d] test case not found" % i)
        elif line.startswith("SKIP"):
            ready = True
            total_skipped += 1
        elif line.startswith("START"):
            total_started += 1
        vm['out'] += line + '\n'
        lines.append(line)
    vm['pending'] = pending
    return ready

def show_progress(scr):
    global num_servers
    global vm
    global dir
    global timestamp
    global tests
    global first_run_failures
    global total_started, total_passed, total_failed, total_skipped

    total_tests = len(tests)
    logger.info("Total tests: %d" % total_tests)

    scr.leaveok(1)
    scr.addstr(0, 0, "Parallel test execution status", curses.A_BOLD)
    for i in range(0, num_servers):
        scr.addstr(i + 1, 0, "VM %d:" % (i + 1), curses.A_BOLD)
        scr.addstr(i + 1, 10, "starting VM")
    scr.addstr(num_servers + 1, 0, "Total:", curses.A_BOLD)
    scr.addstr(num_servers + 1, 20, "TOTAL={} STARTED=0 PASS=0 FAIL=0 SKIP=0".format(total_tests))
    scr.refresh()

    completed_first_pass = False
    rerun_tests = []

    while True:
        running = False
        first_running = False
        updated = False

        for i in range(0, num_servers):
            if completed_first_pass:
                continue
            if vm[i]['first_run_done']:
                continue
            if not vm[i]['proc']:
                continue
            if vm[i]['proc'].poll() is not None:
                vm[i]['proc'] = None
                scr.move(i + 1, 10)
                scr.clrtoeol()
                log = '{}/{}.srv.{}/console'.format(dir, timestamp, i + 1)
                with open(log, 'r') as f:
                    if "Kernel panic" in f.read():
                        scr.addstr("kernel panic")
                        logger.info("VM[%d] kernel panic" % i)
                    else:
                        scr.addstr("unexpected exit")
                        logger.info("VM[%d] unexpected exit" % i)
                updated = True
                continue

            running = True
            first_running = True
            try:
                err = vm[i]['proc'].stderr.read()
                vm[i]['err'] += err
                logger.debug("VM[%d] stderr.read[%s]" % (i, err))
            except:
                pass

            if vm_read_stdout(vm[i], i):
                scr.move(i + 1, 10)
                scr.clrtoeol()
                updated = True
                if not tests:
                    vm[i]['first_run_done'] = True
                    scr.addstr("completed first round")
                    logger.info("VM[%d] completed first round" % i)
                    continue
                else:
                    name = tests.pop(0)
                    vm[i]['proc'].stdin.write(name + '\n')
                    scr.addstr(name)
                    logger.debug("VM[%d] start test %s" % (i, name))

        if not first_running and not completed_first_pass:
            logger.info("First round of testing completed")
            if tests:
                logger.info("Unexpected test cases remaining from first round: " + str(tests))
                raise Exception("Unexpected test cases remaining from first round")
            completed_first_pass = True
            for name in get_failed(vm):
                if rerun_failures:
                    rerun_tests.append(name)
                first_run_failures.append(name)

        for i in range(num_servers):
            if not completed_first_pass:
                continue
            if not vm[i]['proc']:
                continue
            if vm[i]['proc'].poll() is not None:
                vm[i]['proc'] = None
                scr.move(i + 1, 10)
                scr.clrtoeol()
                log = '{}/{}.srv.{}/console'.format(dir, timestamp, i + 1)
                with open(log, 'r') as f:
                    if "Kernel panic" in f.read():
                        scr.addstr("kernel panic")
                        logger.info("VM[%d] kernel panic" % i)
                    else:
                        scr.addstr("completed run")
                        logger.info("VM[%d] completed run" % i)
                updated = True
                continue

            running = True
            try:
                err = vm[i]['proc'].stderr.read()
                vm[i]['err'] += err
                logger.debug("VM[%d] stderr.read[%s]" % (i, err))
            except:
                pass

            ready = False
            if vm[i]['first_run_done']:
                vm[i]['first_run_done'] = False
                ready = True
            else:
                ready = vm_read_stdout(vm[i], i)
            if ready:
                scr.move(i + 1, 10)
                scr.clrtoeol()
                updated = True
                if not rerun_tests:
                    vm[i]['proc'].stdin.write('\n')
                    scr.addstr("shutting down")
                    logger.info("VM[%d] shutting down" % i)
                else:
                    name = rerun_tests.pop(0)
                    vm[i]['proc'].stdin.write(name + '\n')
                    scr.addstr(name + "(*)")
                    logger.debug("VM[%d] start test %s (*)" % (i, name))

        if not running:
            break

        if updated:
            scr.move(num_servers + 1, 10)
            scr.clrtoeol()
            scr.addstr("{} %".format(int(100.0 * (total_passed + total_failed + total_skipped) / total_tests)))
            scr.addstr(num_servers + 1, 20, "TOTAL={} STARTED={} PASS={} FAIL={} SKIP={}".format(total_tests, total_started, total_passed, total_failed, total_skipped))
            failed = get_failed(vm)
            if len(failed) > 0:
                scr.move(num_servers + 2, 0)
                scr.clrtoeol()
                scr.addstr("Failed test cases: ")
                count = 0
                for f in failed:
                    count += 1
                    if count > 30:
                        scr.addstr('...')
                        scr.clrtoeol()
                        break
                    scr.addstr(f)
                    scr.addstr(' ')

            scr.move(0, 35)
            scr.clrtoeol()
            if rerun_tests:
                scr.addstr("(RETRY FAILED %d)" % len(rerun_tests))
            elif rerun_failures:
                pass
            elif first_run_failures:
                scr.addstr("(RETRY FAILED)")

            scr.refresh()

        time.sleep(0.25)

    scr.refresh()
    time.sleep(0.3)

def main():
    global num_servers
    global vm
    global dir
    global timestamp
    global tests
    global first_run_failures
    global total_started, total_passed, total_failed, total_skipped
    global rerun_failures

    total_started = 0
    total_passed = 0
    total_failed = 0
    total_skipped = 0

    debug_level = logging.INFO
    rerun_failures = True

    if len(sys.argv) < 2:
        sys.exit("Usage: %s <number of VMs> [-1] [--debug] [--codecov] [params..]" % sys.argv[0])
    num_servers = int(sys.argv[1])
    if num_servers < 1:
        sys.exit("Too small number of VMs")

    timestamp = int(time.time())

    idx = 2

    if len(sys.argv) > idx and sys.argv[idx] == "-1":
        idx += 1
        rerun_failures = False

    if len(sys.argv) > idx and sys.argv[idx] == "--debug":
        idx += 1
        debug_level = logging.DEBUG

    if len(sys.argv) > idx and sys.argv[idx] == "--codecov":
        idx += 1
        print "Code coverage - build separate binaries"
        logdir = "/tmp/hwsim-test-logs/" + str(timestamp)
        os.makedirs(logdir)
        subprocess.check_call(['./build-codecov.sh', logdir])
        codecov_args = ['--codecov_dir', logdir]
        codecov = True
    else:
        codecov_args = []
        codecov = False

    first_run_failures = []
    tests = []
    cmd = [ '../run-tests.py', '-L' ] + sys.argv[idx:]
    lst = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    for l in lst.stdout.readlines():
        name = l.split(' ')[0]
        tests.append(name)
    if len(tests) == 0:
        sys.exit("No test cases selected")
    if '-f' in sys.argv[idx:]:
        extra_args = sys.argv[idx:]
    else:
        extra_args = [x for x in sys.argv[idx:] if x not in tests]

    dir = '/tmp/hwsim-test-logs'
    try:
        os.mkdir(dir)
    except:
        pass

    if "--shuffle-tests" in extra_args:
        from random import shuffle
        shuffle(tests)
    elif num_servers > 2 and len(tests) > 100:
        # Move test cases with long duration to the beginning as an
        # optimization to avoid last part of the test execution running a long
        # duration test case on a single VM while all other VMs have already
        # completed their work.
        long = [ "ap_roam_open",
                 "wpas_mesh_password_mismatch_retry",
                 "wpas_mesh_password_mismatch",
                 "hostapd_oom_wpa2_psk_connect",
                 "ap_hs20_fetch_osu_stop",
                 "ap_roam_wpa2_psk",
                 "ibss_wpa_none_ccmp",
                 "nfc_wps_er_handover_pk_hash_mismatch_sta",
                 "go_neg_peers_force_diff_freq",
                 "p2p_cli_invite",
                 "sta_ap_scan_2b",
                 "ap_pmf_sta_unprot_deauth_burst",
                 "ap_bss_add_remove_during_ht_scan",
                 "wext_scan_hidden",
                 "autoscan_exponential",
                 "nfc_p2p_client",
                 "wnm_bss_keep_alive",
                 "ap_inactivity_disconnect",
                 "scan_bss_expiration_age",
                 "autoscan_periodic",
                 "discovery_group_client",
                 "concurrent_p2pcli",
                 "ap_bss_add_remove",
                 "wpas_ap_wps",
                 "wext_pmksa_cache",
                 "ibss_wpa_none",
                 "ap_ht_40mhz_intolerant_ap",
                 "ibss_rsn",
                 "discovery_pd_retries",
                 "ap_wps_setup_locked_timeout",
                 "ap_vht160",
                 "dfs_radar",
                 "dfs",
                 "grpform_cred_ready_timeout",
                 "hostapd_oom_wpa2_eap_connect",
                 "wpas_ap_dfs",
                 "ap_wps_pbc_timeout" ]
        for l in long:
            if l in tests:
                tests.remove(l)
                tests.insert(0, l)

    logger.setLevel(debug_level)
    log_handler = logging.FileHandler('parallel-vm.log')
    log_handler.setLevel(debug_level)
    fmt = "%(asctime)s %(levelname)s %(message)s"
    log_formatter = logging.Formatter(fmt)
    log_handler.setFormatter(log_formatter)
    logger.addHandler(log_handler)

    vm = {}
    for i in range(0, num_servers):
        print("\rStarting virtual machine {}/{}".format(i + 1, num_servers)),
        logger.info("Starting virtual machine {}/{}".format(i + 1, num_servers))
        cmd = ['./vm-run.sh', '--delay', str(i), '--timestamp', str(timestamp),
               '--ext', 'srv.%d' % (i + 1),
               '-i'] + codecov_args + extra_args
        vm[i] = {}
        vm[i]['first_run_done'] = False
        vm[i]['proc'] = subprocess.Popen(cmd,
                                         stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE)
        vm[i]['out'] = ""
        vm[i]['pending'] = ""
        vm[i]['err'] = ""
        vm[i]['failed'] = []
        for stream in [ vm[i]['proc'].stdout, vm[i]['proc'].stderr ]:
            fd = stream.fileno()
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    print

    curses.wrapper(show_progress)

    with open('{}/{}-parallel.log'.format(dir, timestamp), 'w') as f:
        for i in range(0, num_servers):
            f.write('VM {}\n{}\n{}\n'.format(i, vm[i]['out'], vm[i]['err']))

    failed = get_failed(vm)

    if first_run_failures:
        print "Failed test cases:"
        for f in first_run_failures:
            print f,
            logger.info("Failed: " + f)
        print
    double_failed = []
    for name in failed:
        double_failed.append(name)
    for test in first_run_failures:
        double_failed.remove(test)
    if not rerun_failures:
        pass
    elif failed and not double_failed:
        print "All failed cases passed on retry"
        logger.info("All failed cases passed on retry")
    elif double_failed:
        print "Failed even on retry:"
        for f in double_failed:
            print f,
            logger.info("Failed on retry: " + f)
        print
    res = "TOTAL={} PASS={} FAIL={} SKIP={}".format(total_started,
                                                    total_passed,
                                                    total_failed,
                                                    total_skipped)
    print(res)
    logger.info(res)
    print "Logs: " + dir + '/' + str(timestamp)
    logger.info("Logs: " + dir + '/' + str(timestamp))

    for i in range(0, num_servers):
        if len(vm[i]['pending']) > 0:
            logger.info("Unprocessed stdout from VM[%d]: '%s'" %
                        (i, vm[i]['pending']))
        log = '{}/{}.srv.{}/console'.format(dir, timestamp, i + 1)
        with open(log, 'r') as f:
            if "Kernel panic" in f.read():
                print "Kernel panic in " + log
                logger.info("Kernel panic in " + log)

    if codecov:
        print "Code coverage - preparing report"
        for i in range(num_servers):
            subprocess.check_call(['./process-codecov.sh',
                                   logdir + ".srv.%d" % (i + 1),
                                   str(i)])
        subprocess.check_call(['./combine-codecov.sh', logdir])
        print "file://%s/index.html" % logdir
        logger.info("Code coverage report: file://%s/index.html" % logdir)

    if double_failed or (failed and not rerun_failures):
        logger.info("Test run complete - failures found")
        sys.exit(2)
    if failed:
        logger.info("Test run complete - failures found on first run; passed on retry")
        sys.exit(1)
    logger.info("Test run complete - no failures")
    sys.exit(0)

if __name__ == "__main__":
    main()
