#include "scheduler.h"
#include "main.h"

#include <stddef.h>

/* Task array (Delta List) */
static SCH_Task tasks[SCH_MAX_TASKS];

/* Số tick 10ms từ ngắt Timer báo xuống */
static volatile uint32_t pending_ticks = 0;

/* Thời gian chuẩn của Scheduler (ms) dùng cho get_time() */
static volatile uint32_t scheduler_time = 0;


/* =====================================================
 * CRITICAL SECTION
 * ===================================================== */
static uint32_t SCH_EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void SCH_ExitCritical(uint32_t primask)
{
    __set_PRIMASK(primask);
}


/* =====================================================
 * FIND FREE TASK ID
 * ===================================================== */
static uint32_t SCH_FindFreeIDLocked(void)
{
    for (uint32_t candidate = 0; candidate < SCH_MAX_TASKS; candidate++)
    {
        uint8_t used = 0;

        for (uint32_t i = 0; i < SCH_MAX_TASKS; i++)
        {
            if (tasks[i].functionPointer != NULL &&
                tasks[i].id == candidate)
            {
                used = 1;
                break;
            }
        }

        if (!used)
        {
            return candidate;
        }
    }

    return SCH_TASK_ERROR;
}


/* =====================================================
 * INTERNAL DELETE
 * ===================================================== */
static uint8_t SCH_DeleteTaskLocked(uint32_t taskID)
{
    for (uint32_t i = 0; i < SCH_MAX_TASKS; i++)
    {
        if (tasks[i].functionPointer != NULL &&
            tasks[i].id == taskID)
        {
            /* Cộng dồn delay cho task phía sau trong Delta List */
            if (i + 1 < SCH_MAX_TASKS &&
                tasks[i + 1].functionPointer != NULL)
            {
                tasks[i + 1].delay += tasks[i].delay;
            }

            /* Dịch các task còn lại lên */
            for (uint32_t j = i; j < SCH_MAX_TASKS - 1; j++)
            {
                tasks[j] = tasks[j + 1];
            }

            /* Xóa ô cuối */
            tasks[SCH_MAX_TASKS - 1].functionPointer = NULL;
            tasks[SCH_MAX_TASKS - 1].id = 0;
            tasks[SCH_MAX_TASKS - 1].delay = 0;
            tasks[SCH_MAX_TASKS - 1].period = 0;
            tasks[SCH_MAX_TASKS - 1].flag = 0;

            return 1;
        }
    }

    return 0;
}


/* =====================================================
 * INTERNAL ADD TASK (Delta List)
 * ===================================================== */
static uint32_t SCH_AddTaskLocked(
    void (*pFunction)(void),
    uint32_t DELAY,
    uint32_t PERIOD
)
{
    if (pFunction == NULL)
    {
        return SCH_TASK_ERROR;
    }

    if (tasks[SCH_MAX_TASKS - 1].functionPointer != NULL)
    {
        return SCH_TASK_ERROR;
    }

    uint32_t newID = SCH_FindFreeIDLocked();
    if (newID == SCH_TASK_ERROR)
    {
        return SCH_TASK_ERROR;
    }

    uint32_t elapsed = 0;
    uint32_t index = 0;

    /* Tìm vị trí chèn theo thứ tự thời gian (giữ nguyên thứ tự nếu cùng deadline) */
    while (index < SCH_MAX_TASKS &&
           tasks[index].functionPointer != NULL)
    {
        uint32_t deadline = elapsed + tasks[index].delay;

        if (deadline > DELAY)
        {
            break;
        }

        elapsed = deadline;
        index++;
    }

    if (index >= SCH_MAX_TASKS)
    {
        return SCH_TASK_ERROR;
    }

    uint32_t newDelay = DELAY - elapsed;

    if (tasks[index].functionPointer != NULL)
    {
        uint32_t oldDelay = tasks[index].delay;

        for (uint32_t j = SCH_MAX_TASKS - 1; j > index; j--)
        {
            tasks[j] = tasks[j - 1];
        }

        tasks[index + 1].delay = oldDelay - newDelay;
    }

    tasks[index].functionPointer = pFunction;
    tasks[index].id = newID;
    tasks[index].delay = newDelay;
    tasks[index].period = PERIOD;
    tasks[index].flag = (newDelay == 0) ? 1 : 0;

    return newID;
}


/* =====================================================
 * SCH INIT
 * ===================================================== */
void SCH_Init(void)
{
    uint32_t primask = SCH_EnterCritical();

    scheduler_time = 0;
    pending_ticks = 0;

    for (uint32_t i = 0; i < SCH_MAX_TASKS; i++)
    {
        tasks[i].functionPointer = NULL;
        tasks[i].id = 0;
        tasks[i].delay = 0;
        tasks[i].period = 0;
        tasks[i].flag = 0;
    }

    SCH_ExitCritical(primask);
}


/* =====================================================
 * SCH UPDATE
 * Gọi mỗi 10ms trong ngắt TIM2 - Độ phức tạp O(1)
 * ===================================================== */
void SCH_Update(void)
{
    pending_ticks++;
}


/* =====================================================
 * SCH ADD TASK
 * ===================================================== */
uint32_t SCH_Add_Task(
    void (*pFunction)(void),
    uint32_t DELAY,
    uint32_t PERIOD
)
{
    uint32_t primask = SCH_EnterCritical();

    uint32_t result = SCH_AddTaskLocked(
        pFunction,
        DELAY,
        PERIOD
    );

    SCH_ExitCritical(primask);

    return result;
}


/* =====================================================
 * SCH DELETE TASK
 * ===================================================== */
uint8_t SCH_Delete_Task(uint32_t taskID)
{
    uint32_t primask = SCH_EnterCritical();

    uint8_t result = SCH_DeleteTaskLocked(taskID);

    SCH_ExitCritical(primask);

    return result;
}


/* =====================================================
 * SCH DISPATCH TASKS
 * Xử lý từng tick 10ms tuần tự: đảm bảo in 0, 10, 20, 30...
 * ===================================================== */
void SCH_Dispatch_Tasks(void)
{
    while (1)
    {
        /* 1. Chạy tất cả các task đã tới hạn (delay == 0) tại mốc thời gian hiện tại */
        while (tasks[0].functionPointer != NULL && tasks[0].delay == 0)
        {
            SCH_Task currentTask;
            uint8_t rescheduleError = 0;

            uint32_t primask = SCH_EnterCritical();

            currentTask = tasks[0];
            SCH_DeleteTaskLocked(currentTask.id);

            if (currentTask.period > 0)
            {
                if (SCH_AddTaskLocked(
                        currentTask.functionPointer,
                        currentTask.period,
                        currentTask.period
                    ) == SCH_TASK_ERROR)
                {
                    rescheduleError = 1;
                }
            }

            SCH_ExitCritical(primask);

            if (rescheduleError)
            {
                Error_Handler();
            }

            /* Thực thi task (in ra đúng mốc scheduler_time hiện tại) */
            currentTask.functionPointer();
        }

        /* 2. Kiểm tra xem có tick 10ms mới từ Timer 2 chưa */
        if (pending_ticks > 0)
        {
            uint32_t primask = SCH_EnterCritical();

            pending_ticks--;
            scheduler_time += SCH_TIMERTICK;

            if (tasks[0].functionPointer != NULL)
            {
                if (tasks[0].delay > SCH_TIMERTICK)
                {
                    tasks[0].delay -= SCH_TIMERTICK;
                }
                else
                {
                    tasks[0].delay = 0;
                    tasks[0].flag = 1;
                }
            }

            SCH_ExitCritical(primask);
        }
        else
        {
            /* Không còn task hay tick nào cần xử lý -> thoát ra */
            break;
        }
    }
}


/* =====================================================
 * GET TIME
 * ===================================================== */
uint32_t get_time(void)
{
    return scheduler_time;
}
