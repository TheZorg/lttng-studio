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
#include <cstdint>

#include <babeltrace/babeltrace.h>
#include <babeltrace/format.h>
#include <babeltrace/context.h>
#include <babeltrace/iterator.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/ctf/iterator.h>
#include <babeltrace/ctf/callbacks.h>

#include <glib.h>

#include "lttng-analyzes/common.h"
#include "lttng-analyzes/sched.h"

using namespace std;

int NUM_THREADS = 8;

class ParallelCpuTest : public QObject
{
    Q_OBJECT

public:
    ParallelCpuTest();

private Q_SLOTS:
    void initTestCase();
    void benchmarkSerialCpu();
    void benchmarkParallelCpu();
private:
    QDir traceDir;
    QDir perStreamTraceDir;
    Sched sched;
};

struct map_params {
    QString tracePath;
    struct bt_iter_pos begin_pos;
    struct bt_iter_pos end_pos;
};


ParallelCpuTest::ParallelCpuTest() : sched()
{
}

void ParallelCpuTest::initTestCase()
{
    QStringList path = QStringList() << "/home" << "fabien" << "lttng-traces"
                                        << "sinoscope-20140708-150630";
    traceDir.setPath(path.join(QDir::separator()) + "/kernel");
    perStreamTraceDir.setPath(path.join(QDir::separator()) + "/kernel_per_stream");
}

enum bt_cb_ret handleSchedSwitchWrapper(bt_ctf_event *event, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id;
    char *prev_comm, *next_comm;
    int prev_tid, next_tid;
    char *hostname;
    Sched *sched = (Sched*)privateData;

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        goto error;

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);
    prev_comm = bt_ctf_get_char_array(bt_ctf_get_field(event,
                scope, "_prev_comm"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing prev_comm context info\n");
        goto error;
    }

    next_comm = bt_ctf_get_char_array(bt_ctf_get_field(event,
                scope, "_next_comm"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing next_comm context info\n");
        goto error;
    }

    prev_tid = bt_ctf_get_int64(bt_ctf_get_field(event,
                scope, "_prev_tid"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing prev_tid context info\n");
        goto error;
    }

    next_tid = bt_ctf_get_int64(bt_ctf_get_field(event,
                scope, "_next_tid"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing next_tid context info\n");
        goto error;
    }

    cpu_id = get_cpu_id(event);

    sched->doSwitch(timestamp, cpu_id, prev_tid, next_tid, prev_comm, next_comm, hostname);

    return BT_CB_OK;

error:
    return BT_CB_ERROR_STOP;
}

void ParallelCpuTest::benchmarkSerialCpu()
{
    return;
    QTime timer;
    timer.start();
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
                             &this->sched, 0, handleSchedSwitchWrapper, NULL, NULL, NULL);

    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        err = bt_iter_next(bt_ctf_get_iter(iter));
        if (err) {
            qDebug() << "Error occured while iterating";
            break;
        }
    }

    QVector<Cpu>& cpus = sched.getCpus();
    QHash<int, Process>& tids = sched.getTids();
    uint64_t start = sched.getStart();
    uint64_t end = sched.getEnd();

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
    int elapsed = timer.elapsed();
    qDebug() << "Elapsed for serial : " << elapsed << "ms.";
}

Sched doCount(const map_params &params);
void doReduce(Sched &final, const Sched& intermediate);

void ParallelCpuTest::benchmarkParallelCpu()
{
        QTime timer;
        timer.start();
        QList< struct map_params > paramsList;

        QString path = perStreamTraceDir.absolutePath();

        for (int i = 0; i < NUM_THREADS; i++)
        {
            struct map_params params;
            params.tracePath = path + "/channel0_" + QString::number(i) + ".d";
            params.begin_pos.type = BT_SEEK_BEGIN;
            params.end_pos.type = BT_SEEK_LAST;
            paramsList << params;
        }

        QFuture<Sched> schedFuture = QtConcurrent::mappedReduced(paramsList, doCount, doReduce);

        Sched sched = schedFuture.result();

        QVector<Cpu>& cpus = sched.getCpus();
        QHash<int, Process>& tids = sched.getTids();

        sort(cpus.begin(), cpus.end(), [](const Cpu &a, const Cpu &b) -> bool {
            return a.cpu_ns > b.cpu_ns;
        });

        QVectorIterator<Cpu> sorted(cpus);
        while(sorted.hasNext()) {
            const Cpu &cpu = sorted.next();
            uint64_t total = sched.getEnd() - sched.getStart();
            double cpu_pc = ((double)(cpu.cpu_ns * 100))/((double)total);
            printf("CPU %d: %0.02f%%\n", cpu.id, cpu_pc);
        }
        int elapsed = timer.elapsed();
        qDebug() << "Elapsed for parallel : " << elapsed << "ms.";
}

Sched doCount(const map_params &params)
{
    struct bt_context *ctx;
    struct bt_ctf_iter *iter;
    struct bt_ctf_event *ctf_event;
    int trace_id;
    int err;
    Sched sched;

    QString path = params.tracePath;
    ctx = bt_context_create();
    trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return sched;
    }

    iter = bt_ctf_iter_create(ctx, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sched_switch"),
                             &sched, 0, handleSchedSwitchWrapper, NULL, NULL, NULL);

    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        err = bt_iter_next(bt_ctf_get_iter(iter));
        if (err) {
            qDebug() << "Error occured while iterating";
            break;
        }
    }

    QVector<Cpu>& cpus = sched.getCpus();
    QHash<int, Process>& tids = sched.getTids();
    uint64_t start = sched.getStart();
    uint64_t end = sched.getEnd();

    QMutableVectorIterator<Cpu> cpu_iter(cpus);
    while (cpu_iter.hasNext()) {
        Cpu &cpu = cpu_iter.next();
        uint64_t total = end - start;
        if (cpu.task_start != 0) {
            cpu.cpu_ns += end - cpu.task_start;
        }
        if (cpu.current_tid >= 0) {
            tids[cpu.current_tid].cpu_ns += end - cpu.task_start;
        }
    }

    sort(cpus.begin(), cpus.end(), [](const Cpu &a, const Cpu &b) -> bool {
        return a.id > b.id;
    });

    bt_ctf_iter_destroy(iter);
    bt_context_put(ctx);

    return sched;
}

void doReduce(Sched &final, const Sched& intermediate)
{
    const QVector<Cpu> &partCpus = intermediate.getCpus();
    const QHash<int, Process> &partTids = intermediate.getTids();
    QVector<Cpu> &totalCpus = final.getCpus();
    QHash<int, Process> &totalTids = final.getTids();

    // Fix start/end times
    if (intermediate.getStart() < final.getStart() || final.getStart() == 0) {
        final.setStart(intermediate.getStart());
    }
    if (intermediate.getEnd() > final.getEnd()) {
        final.setEnd(intermediate.getEnd());
    }

    // Merge cpus (assumes sorted)
    QVectorIterator<Cpu> partCpuIter(partCpus);
    QMutableVectorIterator<Cpu> totalCpuIter(totalCpus);
    while(partCpuIter.hasNext()) {
        const Cpu &partCpu = partCpuIter.next();
        bool found = false;
        while (totalCpuIter.hasNext()) {
            Cpu &totalCpu = totalCpuIter.peekNext();
            if (totalCpu.id == partCpu.id) {
                found = true;
                totalCpu.cpu_ns += partCpu.cpu_ns;
            }
            if (totalCpu.id >= partCpu.id) {
                break;
            }
            totalCpuIter.next();
        }
        if (!found) {
            totalCpuIter.insert(partCpu);
        }
    }
}

QTEST_APPLESS_MAIN(ParallelCpuTest)

#include "tst_parallelcputest.moc"
