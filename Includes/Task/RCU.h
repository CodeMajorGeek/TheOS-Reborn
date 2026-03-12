#ifndef _RCU_H
#define _RCU_H

#include <Debug/Spinlock.h>

#include <stdbool.h>
#include <stdint.h>

#define RCU_SYNC_TIMEOUT_TICKS 100000ULL
#define RCU_CPU_SLOTS          256U

typedef void (*rcu_callback_fn_t)(void* context);

typedef struct rcu_callback_node
{
    struct rcu_callback_node* next;
    rcu_callback_fn_t fn;
    void* context;
    uint64_t target_gp;
} rcu_callback_node_t;

typedef struct rcu_cpu_state
{
    uint32_t read_depth;
    uint32_t reserved;
    uint64_t seen_gp;
} rcu_cpu_state_t;

typedef struct rcu_stats
{
    uint64_t gp_seq;
    uint64_t gp_target;
    uint64_t callbacks_pending;
    uint32_t local_read_depth;
    uint32_t local_preempt_count;
} rcu_stats_t;

typedef struct rcu_runtime_state
{
    spinlock_t lock;
    rcu_cpu_state_t cpu[RCU_CPU_SLOTS];
    uint64_t gp_seq;
    uint64_t gp_target;
    rcu_callback_node_t* cb_head;
    rcu_callback_node_t* cb_tail;
    uint64_t cb_pending;
    bool ready;
} rcu_runtime_state_t;

void RCU_init(void);
void RCU_note_quiescent_state(void);

void RCU_read_lock(void);
void RCU_read_unlock(void);

bool RCU_call(rcu_callback_fn_t fn, void* context);
bool RCU_synchronize(void);
void RCU_get_stats(rcu_stats_t* out_stats);

#endif
