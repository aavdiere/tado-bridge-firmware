#ifndef FAULTS_H
#define FAULTS_H

/* Enable MemManage, BusFault and UsageFault so they are not escalated to a
 * silent HardFault. The handlers themselves are defined in faults.c. */
void faults_init(void);

#endif
