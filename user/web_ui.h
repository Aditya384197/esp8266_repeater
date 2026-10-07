#ifndef _WEB_UI_H_
#define _WEB_UI_H_

/* Web dashboard (status, uplink/AP settings, scan, restart).
 * Called from the espconn callbacks registered in user_main.c */
void web_ui_recv(void *arg, char *data, unsigned short length);
void web_ui_sent(void *arg);
void web_ui_discon(void *arg);

#endif /* _WEB_UI_H_ */
