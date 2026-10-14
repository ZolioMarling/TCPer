#ifndef _ASYNC_LOG_INTERNAL_H_
#define _ASYNC_LOG_INTERNAL_H_

int __async_log_should_flush__(int level);
void __async_log_record_output_failure__(void);

#endif
