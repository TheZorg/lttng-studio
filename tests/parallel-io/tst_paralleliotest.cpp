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

enum class IOType { NONE, UNKNOWN, READ, WRITE };
static const std::string IOTypeStrings[] = { "NONE", "UNKNOWN", "READ", "WRITE" };

struct Syscall {
    IOType type;
    std::string name;
    uint64_t start;
    uint64_t end;
    int fd;
    int ret;
    Syscall() : type(IOType::NONE), name(""), start(0), end(0), fd(0), ret(0) {}
};

struct IOData {
    int tid;
    Syscall currentSyscall;
    Syscall unknownSyscall;

    uint64_t totalReadLatency;
    uint64_t readCount;

    uint64_t totalWriteLatency;
    uint64_t writeCount;
    IOData() :
        currentSyscall(), unknownSyscall(), totalReadLatency(0), readCount(0), totalWriteLatency(0), writeCount(0) {}
};

typedef QHash<int, IOData> IODataMap;

class ParallelIOTest : public QObject
{
    Q_OBJECT

public:
    ParallelIOTest();
    void handleSysRead(uint64_t timestamp, uint64_t cpu, int64_t tid, int fd, char *comm);
    void handleSysWrite(uint64_t timestamp, uint64_t cpu, int64_t tid, int fd, char *comm);
    void handleExitSyscall(uint64_t timestamp, uint64_t cpu, int64_t tid, int ret);

private Q_SLOTS:
    void initTestCase();
    void benchmarkSerialIO();
    void benchmarkParallelIO();

private:
    QDir traceDir;
    QDir perStreamTraceDir;
    QHash<int, IOData> perTidData;
};

struct MapParams {
    TraceWrapper wrapper;
    bt_iter_pos begin_pos;
    bt_iter_pos end_pos;
    IODataMap dataMap;

    MapParams(QString path, bt_iter_pos begin, bt_iter_pos end) :
        wrapper(path), begin_pos(begin), end_pos(end), dataMap() {}

    MapParams(const MapParams &other) = delete;

    MapParams(MapParams &&other) :
        wrapper(std::move(other.wrapper)),
        begin_pos(std::move(other.begin_pos)), end_pos(std::move(other.end_pos)),
        dataMap(std::move(other.dataMap)) {}
};

IODataMap doMap(MapParams &params);
void doReduce(IODataMap &final, const IODataMap &map);

ParallelIOTest::ParallelIOTest()
{
}

void ParallelIOTest::initTestCase()
{
    QStringList path = QStringList() << "/home" << "fabien" << "lttng-traces"
                                     << "redis_tid-20141104-143623";
    //traceDir.setPath(path.join(QDir::separator()) + "/kernel_per_stream/channel0_3.d");
    traceDir.setPath(path.join(QDir::separator()) + "/kernel");
    perStreamTraceDir.setPath(path.join(QDir::separator()) + "/kernel_per_stream");
}

uint64_t get_context_tid(const struct bt_ctf_event *event)
{
    const struct bt_definition *scope;
    uint64_t tid;

    scope = bt_ctf_get_top_level_scope(event, BT_STREAM_EVENT_CONTEXT);
    tid = bt_ctf_get_int64(bt_ctf_get_field(event,
                scope, "_tid"));
    if (bt_ctf_field_get_error()) {
        tid = bt_ctf_get_int64(bt_ctf_get_field(event,
                    scope, "_vtid"));
        if (bt_ctf_field_get_error()) {
            fprintf(stderr, "Missing tid context info\n");
            return -1ULL;
        }
    }

    return tid;
}

char *get_context_comm(const struct bt_ctf_event *event)
{
    const struct bt_definition *scope;
    char *comm;

    scope = bt_ctf_get_top_level_scope(event, BT_STREAM_EVENT_CONTEXT);
    comm = bt_ctf_get_char_array(bt_ctf_get_field(event,
                scope, "_procname"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing comm context info\n");
        return NULL;
    }

    return comm;
}

enum bt_cb_ret handleSysReadWrapper(bt_ctf_event *event, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id;
    int64_t tid;
    int fd;
    char *procname;
    IODataMap &perTidData = *(IODataMap*)privateData;

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        return BT_CB_ERROR_STOP;

    tid = get_context_tid(event);

    procname = get_context_comm(event);

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);

    fd = bt_ctf_get_uint64(bt_ctf_get_field(event,
                scope, "_fd"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing some context info\n");
        return BT_CB_ERROR_STOP;
    }

    cpu_id = get_cpu_id(event);

    if (!perTidData.contains(tid)) {
        IOData d;
        d.tid = tid;
        perTidData[tid] = d;
    }
    IOData &d = perTidData[tid];
    Syscall &s = d.currentSyscall;
    s.type = IOType::READ;
    s.start = timestamp;
    s.fd = fd;

    return BT_CB_OK;
}

enum bt_cb_ret handleSysWriteWrapper(bt_ctf_event *event, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id;
    int64_t tid;
    int fd;
    char *procname;
    IODataMap &perTidData = *(IODataMap*)privateData;

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        return BT_CB_ERROR_STOP;

    tid = get_context_tid(event);

    procname = get_context_comm(event);

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);

    fd = bt_ctf_get_uint64(bt_ctf_get_field(event,
                scope, "_fd"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing some context info\n");
        return BT_CB_ERROR_STOP;
    }

    cpu_id = get_cpu_id(event);

    if (!perTidData.contains(tid)) {
        IOData d;
        d.tid = tid;
        perTidData[tid] = d;
    }
    IOData &d = perTidData[tid];
    Syscall &s = d.currentSyscall;
    s.type = IOType::WRITE;
    s.start = timestamp;
    s.fd = fd;

    return BT_CB_OK;
}

enum bt_cb_ret handleExitSyscallWrapper(bt_ctf_event *event, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id;
    int64_t tid;
    int64_t ret;
    char *procname;
    IODataMap &perTidData = *(IODataMap*)privateData;

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        return BT_CB_ERROR_STOP;

    tid = get_context_tid(event);

    procname = get_context_comm(event);

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);

    ret = bt_ctf_get_int64(bt_ctf_get_field(event,
                    scope, "_ret"));

    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing some context info\n");
        return BT_CB_ERROR_STOP;
    }

    cpu_id = get_cpu_id(event);

    if (!perTidData.contains(tid)) {
        IOData d;
        d.tid = tid;
        perTidData[tid] = d;
    }
    IOData &d = perTidData[tid];
    Syscall &s = d.currentSyscall;
    uint64_t latency = timestamp - s.start;
    switch (s.type) {
    case IOType::READ:
        d.totalReadLatency += latency;
        d.readCount++;
        s.type = IOType::NONE;
        break;
    case IOType::WRITE:
        d.totalWriteLatency += latency;
        d.writeCount++;
        s.type = IOType::NONE;
        break;
    default:
        Syscall &u = d.unknownSyscall;
        if (u.type == IOType::NONE) {
            u.type = IOType::UNKNOWN;
            u.end = timestamp;
            u.ret = ret;
        }
        break;
    }

    return BT_CB_OK;
}

void ParallelIOTest::handleSysRead(uint64_t timestamp, uint64_t cpu, int64_t tid, int fd, char *comm) {
    if (!perTidData.contains(tid)) {
        IOData d;
        d.tid = tid;
        perTidData[tid] = d;
    }
    IOData &d = perTidData[tid];
    Syscall &s = d.currentSyscall;
    s.type = IOType::READ;
    s.start = timestamp;
    s.fd = fd;
}

void ParallelIOTest::handleSysWrite(uint64_t timestamp, uint64_t cpu, int64_t tid, int fd, char *comm) {
    if (!perTidData.contains(tid)) {
        IOData d;
        d.tid = tid;
        perTidData[tid] = d;
    }
    IOData &d = perTidData[tid];
    Syscall &s = d.currentSyscall;
    s.type = IOType::WRITE;
    s.start = timestamp;
    s.fd = fd;
}

void ParallelIOTest::handleExitSyscall(uint64_t timestamp, uint64_t cpu, int64_t tid, int ret) {
    if (!perTidData.contains(tid)) {
        IOData d;
        d.tid = tid;
        perTidData[tid] = d;
    }
    IOData &d = perTidData[tid];
    Syscall &s = d.currentSyscall;
    uint64_t latency = timestamp - s.start;
    switch (s.type) {
    case IOType::READ:
        d.totalReadLatency += latency;
        d.readCount++;
        s.type = IOType::UNKNOWN;
        break;
    case IOType::WRITE:
        d.totalWriteLatency = latency;
        d.writeCount++;
        s.type = IOType::UNKNOWN;
        break;
    default:
        break;
    }
}

void ParallelIOTest::benchmarkSerialIO()
{
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
                             g_quark_from_static_string("sys_write"),
                             &this->perTidData, 0, handleSysWriteWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_writev"),
                             &this->perTidData, 0, handleSysWriteWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_read"),
                             &this->perTidData, 0, handleSysReadWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_readv"),
                             &this->perTidData, 0, handleSysReadWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("exit_syscall"),
                             &this->perTidData, 0, handleExitSyscallWrapper, NULL, NULL, NULL);

    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        err = bt_iter_next(bt_ctf_get_iter(iter));
        if (err) {
            qDebug() << "Error occured while iterating";
            break;
        }
    }

    const QList<IOData> &data = perTidData.values();
    std::sort(data.begin(), data.end(), [](IOData a, IOData b) {
       return a.readCount > b.readCount;
    });
    for (auto i = data.begin(); i != data.end(); i++) {
        double avgReadLatency = i->readCount ? i->totalReadLatency / i->readCount : 0;
        qDebug() << i->tid << avgReadLatency << i->readCount;
    }

    bt_ctf_iter_destroy(iter);
    bt_context_put(ctx);
    int elapsed = timer.elapsed();
    qDebug() << "Elapsed for serial : " << elapsed << "ms.";
}

void ParallelIOTest::benchmarkParallelIO()
{
    for (int threads = NUM_THREADS; threads > 0; threads--) {
        QThreadPool::globalInstance()->setMaxThreadCount(threads);
        QTime timer;
        timer.start();
        struct bt_iter_pos positions[threads+1];
        std::vector<MapParams> paramsList;
        struct bt_iter_pos end_pos;

        QString path = traceDir.absolutePath();

        // Open a trace to get the begin/end timestamps
        struct bt_context *ctx = bt_context_create();
        int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
        if(trace_id < 0)
        {
            qDebug() << "Failed: bt_context_add_trace";
            return;
        }

        end_pos.type = BT_SEEK_LAST;

        // Get begin timestamp
        struct bt_ctf_iter* iter = bt_ctf_iter_create(ctx, NULL, NULL);
        struct bt_ctf_event *event = bt_ctf_iter_read_event(iter);
        uint64_t begin = bt_ctf_get_timestamp(event);

        // Get end timestamp
        bt_iter_set_pos(bt_ctf_get_iter(iter), &end_pos);
        event = bt_ctf_iter_read_event(iter);
        uint64_t end = bt_ctf_get_timestamp(event);

        // We don't need that context anymore, dispose of it
        bt_context_put(ctx);

        // Calculate begin/end timestamp pairs for each chunk
        uint64_t step = (end - begin)/threads;

        positions[0].type = BT_SEEK_BEGIN;
        for (int i = 1; i < threads; i++)
        {
            positions[i].type = BT_SEEK_TIME;
            positions[i].u.seek_time = begin + (i*step);
        }
        positions[threads].type = BT_SEEK_LAST;

        // Build the params list
        for (int i = 0; i < threads; i++)
        {
            paramsList.push_back(MapParams(path, positions[i], positions[i+1]));
        }

        auto future = QtConcurrent::mappedReduced(paramsList.begin(),
                                                            paramsList.end(),
                                                            doMap, doReduce,
                                                            QtConcurrent::OrderedReduce);

        IODataMap finalResults = future.result();

        const auto &values = finalResults.values();
        std::sort(values.begin(), values.end(), [](const IOData &a, const IOData &b) {
           return a.readCount > b.readCount;
        });
        for (auto i = values.begin(); i != values.end(); i++) {
//            double avgReadLatency = i->readCount ? i->totalReadLatency / i->readCount : 0;
//            qDebug() << i->tid << avgReadLatency << i->readCount;
        }
        int elapsed = timer.elapsed();
        qDebug() << "Elapsed for parallel" << threads <<": " << elapsed << "ms.";
    }
}

IODataMap doMap(MapParams &params)
{
    TraceWrapper &wrapper = params.wrapper;
    IODataMap &stateMachines = params.dataMap;
    bt_context *ctx = wrapper.getContext();
    struct bt_ctf_iter *iter;
    struct bt_ctf_event *ctf_event;
    int err;

    iter = bt_ctf_iter_create(ctx, &params.begin_pos, &params.end_pos);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_read"),
                             &stateMachines, 0, handleSysReadWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_readv"),
                             &stateMachines, 0, handleSysReadWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_write"),
                             &stateMachines, 0, handleSysWriteWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("sys_writev"),
                             &stateMachines, 0, handleSysWriteWrapper, NULL, NULL, NULL);
    bt_ctf_iter_add_callback(iter,
                             g_quark_from_static_string("exit_syscall"),
                             &stateMachines, 0, handleExitSyscallWrapper, NULL, NULL, NULL);

    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        err = bt_iter_next(bt_ctf_get_iter(iter));
        if (err) {
            qDebug() << "Error occured while iterating";
            break;
        }
    }

    bt_ctf_iter_destroy(iter);

    return params.dataMap;
}

void doReduce(IODataMap &final, const IODataMap &map) {
    const auto perTidData = map.values();
    for (auto iter = perTidData.begin(); iter != perTidData.end(); iter++) {
        const IOData &data = *iter;
        if(!final.contains(data.tid)) {
            IOData d;
            d.tid = data.tid;
            final[data.tid] = d;
        }
        IOData &finalData = final[data.tid];
        if (data.readCount > 0) {
            finalData.totalReadLatency += data.totalReadLatency;
            finalData.readCount += data.readCount;
        }
        if (data.writeCount > 0) {
            finalData.totalWriteLatency += data.totalWriteLatency;
            finalData.writeCount += data.writeCount;
        }
        if (finalData.currentSyscall.type != IOType::NONE) {
            // We have an unfinished syscall
            if (data.unknownSyscall.type == IOType::UNKNOWN) {
                uint64_t latency = data.unknownSyscall.end - finalData.currentSyscall.start;
                if (finalData.currentSyscall.type == IOType::READ) {
                    finalData.totalReadLatency += latency;
                    finalData.readCount++;
                }
                if (finalData.currentSyscall.type == IOType::WRITE) {
                    finalData.totalWriteLatency += latency;
                    finalData.writeCount++;
                }
            }
        }
        finalData.currentSyscall = data.currentSyscall;
    }
}

QTEST_APPLESS_MAIN(ParallelIOTest)

#include "tst_paralleliotest.moc"
