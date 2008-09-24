/*
 * wpa_gui - WpaGui class
 * Copyright (c) 2005-2006, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#ifndef WPAGUI_H
#define WPAGUI_H

#include <QSystemTrayIcon>
#include <QObject>
#include "ui_wpagui.h"

class UserDataRequest;


class WpaGui : public QMainWindow, public Ui::WpaGui
{
	Q_OBJECT

public:
	WpaGui(QWidget *parent = 0, const char *name = 0,
	       Qt::WFlags fl = 0);
	~WpaGui();

	virtual int ctrlRequest(const char *cmd, char *buf, size_t *buflen);
	virtual void triggerUpdate();
	virtual void editNetwork(const QString &sel);
	virtual void removeNetwork(const QString &sel);
	virtual void enableNetwork(const QString &sel);
	virtual void disableNetwork(const QString &sel);
	virtual int getNetworkDisabled(const QString &sel);

public slots:
	virtual void parse_argv();
	virtual void updateStatus();
	virtual void updateNetworks();
	virtual void fileExit();
	virtual void helpIndex();
	virtual void helpContents();
	virtual void helpAbout();
	virtual void disconnect();
	virtual void scan();
	virtual void eventHistory();
	virtual void ping();
	virtual void processMsg(char *msg);
	virtual void processCtrlReq(const char *req);
	virtual void receiveMsgs();
	virtual void connectB();
	virtual void selectNetwork(const QString &sel);
	virtual void editSelectedNetwork();
	virtual void editListedNetwork();
	virtual void removeSelectedNetwork();
	virtual void removeListedNetwork();
	virtual void addNetwork();
	virtual void enableAllNetworks();
	virtual void disableAllNetworks();
	virtual void removeAllNetworks();
	virtual void saveConfig();
	virtual void selectAdapter(const QString &sel);
	virtual void updateNetworkDisabledStatus();
	virtual void enableListedNetwork(bool);
	virtual void disableListedNetwork(bool);
	virtual void showTrayMessage(QSystemTrayIcon::MessageIcon type,
				     int sec, const QString &msg);
	virtual void showTrayStatus();

protected slots:
	virtual void languageChange();
	virtual void trayActivated(QSystemTrayIcon::ActivationReason how);
	virtual void closeEvent(QCloseEvent *event);

private:
	ScanResults *scanres;
	bool networkMayHaveChanged;
	char *ctrl_iface;
	EventHistory *eh;
	struct wpa_ctrl *ctrl_conn;
	QSocketNotifier *msgNotifier;
	QTimer *timer;
	int pingsToStatusUpdate;
	WpaMsgList msgs;
	char *ctrl_iface_dir;
	struct wpa_ctrl *monitor_conn;
	UserDataRequest *udr;
	QAction *disconnectAction;
	QAction *reconnectAction;
	QAction *eventAction;
	QAction *scanAction;
	QAction *statAction;
	QAction *showAction;
	QAction *hideAction;
	QAction *quitAction;
	QMenu *tray_menu;
	QSystemTrayIcon *tray_icon;
	void createTrayIcon();
	bool ackTrayIcon;

	int openCtrlConnection(const char *ifname);
};

#endif /* WPAGUI_H */
