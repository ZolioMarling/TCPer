#ifndef _USAGE_H_
#define _USAGE_H_

#include "../stdc/os.h"

void usage_logo(void);

void usage_help(char *user_name, char *app_name, int port);

void usage_progress_bar(int64 current, int64 total, int step, char *description);

#endif
