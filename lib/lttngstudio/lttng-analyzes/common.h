#ifndef COMMON_H
#define COMMON_H

#include <QString>
#include <cstdint>
#include <babeltrace/ctf/events.h>


struct Process
{
    int pid;
    int tid;
    uint64_t cpu_ns;
    QString comm;
    uint64_t last_sched;
    Process() : pid(-1), tid(-1), cpu_ns(0), comm(), last_sched(0) {}
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

/*!
 * \brief The TraceWrapper class is used to lazily initialize
 * contexts and add traces when needed.
 */
class TraceWrapper {
private:
    bt_context *ctx;
    QString tracePath;

public:

    TraceWrapper(QString tracePath) : ctx(NULL), tracePath(tracePath) { }

    // Copying isn't allowed
    TraceWrapper(const TraceWrapper &other) = delete;

    // Moving is ok though (C++11)
    TraceWrapper(TraceWrapper &&other) : ctx(std::move(other.ctx)), tracePath(std::move(other.tracePath)) { }

    ~TraceWrapper() {
        if (ctx) {
            bt_context_put(ctx);
        }
    }

    /*!
     * \brief Initialize the context if not initialized
     * and return it.
     * \return The context.
     */
    bt_context* getContext() {
        if (!ctx) {
            ctx = bt_context_create();
            int trace_id = bt_context_add_trace(ctx, tracePath.toStdString().c_str(), "ctf", NULL, NULL, NULL);
            if(trace_id < 0) {
//                qDebug() << "Failed: bt_context_add_trace";
            }
        }
        return ctx;
    }
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
