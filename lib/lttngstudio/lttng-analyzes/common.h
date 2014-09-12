#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <babeltrace/ctf/events.h>


struct Process
{
    int pid;
    int tid;
    uint64_t cpu_ns;
    char *comm;
    uint64_t last_sched;
    Process() : pid(-1), tid(-1), cpu_ns(0), comm(NULL), last_sched(0) {}
};

struct Cpu
{
    unsigned int id;
    uint64_t task_start;
    int current_tid;
    uint64_t cpu_ns;
    double cpu_pc;
    Cpu(int id) : id(id), task_start(0), current_tid(-1), cpu_ns(0), cpu_pc(0.0) {}
    Cpu() : Cpu(0) {}
};

uint64_t get_cpu_id(const struct bt_ctf_event *event)
{
    const struct bt_definition *scope;
    uint64_t cpu_id;

    scope = bt_ctf_get_top_level_scope(event, BT_STREAM_PACKET_CONTEXT);
    cpu_id = bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "cpu_id"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "[error] get cpu_id\n");
        return -1ULL;
    }

    return cpu_id;
}

#endif // COMMON_H
