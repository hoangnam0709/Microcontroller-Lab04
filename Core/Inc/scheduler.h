#ifndef INC_SCHEDULER_H_
#define INC_SCHEDULER_H_

#include <stdint.h>

/* Scheduler configuration */
#define SCH_MAX_TASKS   16U
#define SCH_TIMERTICK   10U

/* Return value when task list is full */
#define SCH_TASK_ERROR  SCH_MAX_TASKS

typedef struct
{
    void (*functionPointer)(void);
    uint32_t id;
    uint32_t delay;
    uint32_t period;
    uint8_t flag;
} SCH_Task;

/* Scheduler functions */
void SCH_Init(void);
void SCH_Update(void);
void SCH_Dispatch_Tasks(void);

uint32_t SCH_Add_Task(
    void (*pFunction)(void),
    uint32_t DELAY,
    uint32_t PERIOD
);

uint8_t SCH_Delete_Task(uint32_t taskID);

/* Additional function required by demonstration */
uint32_t get_time(void);

#endif /* INC_SCHEDULER_H_ */
