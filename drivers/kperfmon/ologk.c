#include <linux/ologk.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

// Define _perflog as a no-op if performance monitoring is disabled
void _perflog(int type, int logid, const char *fmt, ...) {
    // Empty function (no-op)
}

//void perflog_evt(int logid, int arg1) {
//}

