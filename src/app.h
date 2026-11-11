#ifndef APP_H
#define APP_H

/* Creates the radio, net and housekeeping tasks (src/app.c). Call before
 * vTaskStartScheduler(). */
void app_create_tasks(void);

#endif
