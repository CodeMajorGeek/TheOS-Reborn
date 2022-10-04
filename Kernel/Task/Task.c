#include <Task/Task.h>

#include <Memory/VMM.h>
#include <CPU/GDT.h>
#include <CPU/TSS.h>

#include <string.h>
#include <stdio.h>

static TSS_t tss;

void task_init(void)
{
    memset(&tss, 0, sizeof (tss));

    printf("Initialize multitasking !\n");

    GDT_load_TSS_segment(&tss);
    TSS_flush(TSS_SYSTEM_SEGMENT);
}

