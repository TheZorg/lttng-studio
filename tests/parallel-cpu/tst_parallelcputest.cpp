#include <QString>
#include <QtTest>
#include <QDir>
#include <QProcessEnvironment>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <tbb/tbb.h>

#include <iostream>
#include <algorithm>
#include <cstdio>

#include <stdint.h>

#include <babeltrace/babeltrace.h>
#include <babeltrace/format.h>
#include <babeltrace/context.h>
#include <babeltrace/iterator.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/ctf/iterator.h>
#include <babeltrace/ctf/callbacks.h>

#include <glib.h>

using namespace std;

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
    Cpu() : id(0), task_start(0), current_tid(-1), cpu_ns(0), cpu_pc(0.0) {}
};

class ParallelCpuTest : public QObject
{
    Q_OBJECT

public:
    ParallelCpuTest();
    enum bt_cb_ret handleSchedSwitch(bt_ctf_event *call_data, void *privateData);

private Q_SLOTS:
    void initTestCase();
    void benchmarkSerialCpu();
//    void benchmarkParallelCpu();
private:
    QDir traceDir;
    QDir perStreamTraceDir;
    QHash<int, Process> tids;
    QVector<Cpu> cpus;
    uint64_t start;
    uint64_t end;

    void update_cputop_data(uint64_t timestamp, int64_t cpu, int prev_pid,
            int next_pid, char *prev_comm, char *next_comm, char *hostname);

    Cpu& getCpu(unsigned int cpu);
    static uint64_t get_cpu_id(const struct bt_ctf_event *event);
};

ParallelCpuTest *gTest;

ParallelCpuTest::ParallelCpuTest() : start(0), end(0)
{
    gTest = this;
}

void ParallelCpuTest::initTestCase()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
//    QStringList path = QStringList() << env.value("top_srcdir") << "3rdparty"
//                                     << "babeltrace" << "tests" << "ctf-traces"
//                                     << "succeed" << "lttng-modules-2.0-pre5";
    QStringList path = QStringList() << "/home" << "fabien" << "lttng-traces"
                                        << "sinoscope-20140708-150630";
    traceDir.setPath(path.join(QDir::separator()) + "/kernel");
    perStreamTraceDir.setPath(path.join(QDir::separator()) + "/kernel_per_stream");
}

uint64_t ParallelCpuTest::get_cpu_id(const struct bt_ctf_event *event)
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

Cpu& ParallelCpuTest::getCpu(unsigned int cpu)
{
    QMutableVectorIterator<Cpu> iter(cpus);
    while (iter.hasNext()) {
        Cpu &tmp = iter.next();
        if (tmp.id == cpu) {
            return tmp;
        }
    }
    Cpu newCpu;
    newCpu.id = cpu;
    newCpu.current_tid = -1;
    newCpu.task_start = 0;
    cpus.append(newCpu);
    return cpus.last();
}

void ParallelCpuTest::update_cputop_data(uint64_t timestamp, int64_t cpu, int prev_pid,
        int next_pid, char *prev_comm, char *next_comm, char *hostname)
{
    if (start == 0) {
        start = timestamp;
    }
    end = timestamp;

    Cpu &c = getCpu(cpu);
    if (c.task_start != 0) {
        c.cpu_ns += timestamp - c.task_start;
    }

    if (next_pid != 0) {
        c.task_start = timestamp;
        c.current_tid = next_pid;
    } else {
        c.task_start = 0;
        c.current_tid = -1;
    }

    if (tids.contains(prev_pid)) {
        Process &p = tids[prev_pid];
        p.cpu_ns += (timestamp - p.last_sched);
    }

    if (next_pid == 0) {
        return;
    }

    if (!tids.contains(next_pid)) {
        Process p;
        p.tid = next_pid;
        p.comm = next_comm;
        tids[next_pid] = p;
    } else {
        Process &p = tids[next_pid];
        p.comm = next_comm;
    }

}

enum bt_cb_ret handleSchedSwitchWrapper(bt_ctf_event *event, void *privateData)
{
    ParallelCpuTest *test = gTest;
    return test->handleSchedSwitch(event, privateData);
}

enum bt_cb_ret ParallelCpuTest::handleSchedSwitch(bt_ctf_event *call_data, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id;
    char *prev_comm, *next_comm;
    int prev_tid, next_tid;
    char *hostname;

    timestamp = bt_ctf_get_timestamp(call_data);
    if (timestamp == -1ULL)
        goto error;

    scope = bt_ctf_get_top_level_scope(call_data,
            BT_EVENT_FIELDS);
    prev_comm = bt_ctf_get_char_array(bt_ctf_get_field(call_data,
                scope, "_prev_comm"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing prev_comm context info\n");
        goto error;
    }

    next_comm = bt_ctf_get_char_array(bt_ctf_get_field(call_data,
                scope, "_next_comm"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing next_comm context info\n");
        goto error;
    }

    prev_tid = bt_ctf_get_int64(bt_ctf_get_field(call_data,
                scope, "_prev_tid"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing prev_tid context info\n");
        goto error;
    }

    next_tid = bt_ctf_get_int64(bt_ctf_get_field(call_data,
                scope, "_next_tid"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing next_tid context info\n");
        goto error;
    }

    cpu_id = get_cpu_id(call_data);

    update_cputop_data(timestamp, cpu_id, prev_tid, next_tid,
            prev_comm, next_comm, hostname);

    return BT_CB_OK;

error:
    return BT_CB_ERROR_STOP;
}

void ParallelCpuTest::benchmarkSerialCpu()
{
    struct bt_context *ctx;
    struct bt_ctf_iter *iter;
    struct bt_ctf_event *ctf_event;
    int trace_id;
    int err;

    QString path = traceDir.absolutePath();

    ctx = bt_context_create();
    trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return;
    }

    iter = bt_ctf_iter_create(ctx, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sched_switch"),
                             NULL, 0, handleSchedSwitchWrapper, NULL, NULL, NULL);

    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        err = bt_iter_next(bt_ctf_get_iter(iter));
        if (err) {
            qDebug() << "Error occured while iterating";
            break;
        }
    }

    QMutableVectorIterator<Cpu> cpu_iter(cpus);
    while (cpu_iter.hasNext()) {
        Cpu &cpu = cpu_iter.next();
        uint64_t total = end - start;
        if (cpu.task_start != 0) {
            cpu.cpu_ns += end - cpu.task_start;
        }
        uint64_t cpu_total = cpu.cpu_ns;
        cpu.cpu_pc = ((double)(cpu_total * 100))/((double)total);
        if (cpu.current_tid >= 0) {
            tids[cpu.current_tid].cpu_ns += end - cpu.task_start;
        }
    }

    sort(cpus.begin(), cpus.end(), [](const Cpu &a, const Cpu &b) -> bool {
        return a.cpu_ns > b.cpu_ns;
    });

    QVectorIterator<Cpu> sorted(cpus);
    while(sorted.hasNext()) {
        const Cpu &cpu = sorted.next();
        printf("CPU %d: %0.02f%%\n", cpu.id, cpu.cpu_pc);
    }

    bt_ctf_iter_destroy(iter);
    bt_context_put(ctx);
}

QTEST_APPLESS_MAIN(ParallelCpuTest)

#include "tst_parallelcputest.moc"
