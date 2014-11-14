#include <QString>
#include <QtTest>
#include <QDir>
#include <QProcessEnvironment>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <tbb/tbb.h>

#include <iostream>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdint>
#include <array>
#include <random>
#include <type_traits>

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
#include "lttng-analyzes/fsm.h"

using namespace std;

int NUM_THREADS = 8;

//!
//! \brief Enum of FSM states
//!
enum class IOStates : unsigned int {
    IDLE,
    READING,
    WRITING,
    SIZE,
};

//!
//! \brief Enum of FSM events
//!
enum class IOEvents : unsigned int {
    READ,
    WRITE,
    EXIT,
    SIZE,
};

//!
//! \brief Represents a single syscall
//!
struct Syscall {
    std::string name;
    uint64_t start;
    uint64_t end;
    int fd;
    Syscall() : name(""), start(0), end(0), fd(0) {}
};

//!
//! \brief The IOData struct stores the results of
//! running the FSM
//!
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

//!
//! \brief The IOStateMachine class implements the
//! FSM for IO analysis
//!
class IOStateMachine : public BaseStateMachine<IOStates, IOEvents, IOData, IOStateMachine> {
protected:
    friend class BaseStateMachine<IOStates, IOEvents, IOData, IOStateMachine>;

    // Declare all default transitions;
    DEFAULT_TRANSITIONS

    // Declare default transition functions
    template<IOStates S, IOEvents E>
    void doTransitionFunctionSpec(const EventParams<E> &params) { (void)params; }
};

/*******************
 * EVENT PARAMETERS
 *******************/

template<> template<>
struct IOStateMachine::EventParams<IOEvents::READ> : public BaseEventParams {
    uint64_t timestamp;
    int fd;
    int count;
    EventParams<IOEvents::READ>() : timestamp(0), fd(0), count(0) {}
};

template<> template<>
struct IOStateMachine::EventParams<IOEvents::WRITE> : public BaseEventParams {
    uint64_t timestamp;
    int fd;
    int count;
    EventParams<IOEvents::WRITE>() : timestamp(0), fd(0), count(0) {}
};

template<> template<>
struct IOStateMachine::EventParams<IOEvents::EXIT> : public BaseEventParams {
    uint64_t timestamp;
    int ret;
    EventParams<IOEvents::EXIT>() : timestamp(0), ret(0) {}
};

/*******************
 * FSM TRANSITIONS
 *******************/

DEF_TRANSITION(IOStateMachine, IOStates::IDLE, IOEvents::READ, IOStates::READING)
DEF_TRANSITION(IOStateMachine, IOStates::IDLE, IOEvents::WRITE, IOStates::WRITING)
DEF_TRANSITION(IOStateMachine, IOStates::READING, IOEvents::EXIT, IOStates::IDLE)
DEF_TRANSITION(IOStateMachine, IOStates::WRITING, IOEvents::EXIT, IOStates::IDLE)

/***********************
 * TRANSITION FUNCTIONS
 ***********************/

template<>
void IOStateMachine::doTransitionFunctionSpec<IOStates::IDLE, IOEvents::READ>(const EventParams<IOEvents::READ> &params) {
    Syscall &syscall = this->data.currentSyscall;
    syscall.name = "sys_read";
    syscall.start = params.timestamp;
    syscall.fd = params.fd;
}

template<>
void IOStateMachine::doTransitionFunctionSpec<IOStates::IDLE, IOEvents::WRITE>(const EventParams<IOEvents::WRITE> &params) {
    Syscall &syscall = this->data.currentSyscall;
    syscall.name = "sys_write";
    syscall.start = params.timestamp;
    syscall.fd = params.fd;
}

template<>
void IOStateMachine::doTransitionFunctionSpec<IOStates::READING, IOEvents::EXIT>(const EventParams<IOEvents::EXIT> &params) {
    Syscall &syscall = this->data.currentSyscall;
    if (syscall.name.empty()) {
        this->data.unknownSyscall.end = params.timestamp;
        this->data.unknownSyscall.name = "sys_read";
    } else {
        uint64_t latency = params.timestamp - syscall.start;
        this->data.totalReadLatency += latency;
        this->data.readCount++;
        this->data.currentSyscall = Syscall();
    }
}

template<>
void IOStateMachine::doTransitionFunctionSpec<IOStates::WRITING, IOEvents::EXIT>(const EventParams<IOEvents::EXIT> &params) {
    Syscall &syscall = this->data.currentSyscall;
    if (syscall.name.empty()) {
        this->data.unknownSyscall.end = params.timestamp;
        this->data.unknownSyscall.name = "sys_write";
    } else {
        uint64_t latency = params.timestamp - syscall.start;
        this->data.totalWriteLatency += latency;
        this->data.writeCount++;
        this->data.currentSyscall = Syscall();
    }
}

class ParallelStateMachineTest : public QObject
{
    Q_OBJECT

public:
    ParallelStateMachineTest();
    void handleSchedSwitch(uint64_t timestamp, int64_t cpu, int prev_pid, int next_pid,
                            char *prev_comm, char *next_comm, char *hostname);
    void handleSysRead(uint64_t timestamp, uint64_t cpu, int64_t tid, int fd, char *comm);
    void handleSysWrite(uint64_t timestamp, uint64_t cpu, int64_t tid, int fd, char *comm);
    void handleExitSyscall(uint64_t timestamp, uint64_t cpu, int64_t tid, int ret);


private Q_SLOTS:
    void initTestCase();
    void benchmarkSerialStateMachine();
    void benchmarkParallelStateMachine();

private:
    QDir traceDir;
    QDir perStreamTraceDir;
    Sched sched;
    QHash<int, IOStateMachine> stateMachines;
};

typedef QHash<int, IOStateMachine> IOStateMachineMap;
typedef QVector<IOStateMachineMap> IOStateMachineMapList;

struct MapParams {
    TraceWrapper wrapper;
    bt_iter_pos begin_pos;
    bt_iter_pos end_pos;
    IOStateMachineMap stateMachines;

    MapParams(QString path, bt_iter_pos begin, bt_iter_pos end) :
        wrapper(path), begin_pos(begin), end_pos(end), stateMachines() {}

    MapParams(const MapParams &other) = delete;

    MapParams(MapParams &&other) :
        wrapper(std::move(other.wrapper)),
        begin_pos(std::move(other.begin_pos)), end_pos(std::move(other.end_pos)),
        stateMachines(std::move(other.stateMachines)) {}
};

IOStateMachineMap doFirstPassMap(MapParams &params);
void doFirstPassReduce(IOStateMachineMapList &final, const IOStateMachineMap &intermediate);
IOStateMachineMap doSecondPassMap(const MapParams &params);
void doSecondPassReduce(QHash<int, IOData> &final, const IOStateMachineMap &map);

ParallelStateMachineTest::ParallelStateMachineTest() {

}

void ParallelStateMachineTest::initTestCase()
{
    QStringList path = QStringList() << "/home" << "fabien" << "lttng-traces"
//                                        << "fsm-test-20141029-180956";
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
    uint64_t cpu_id; (void) cpu_id;
    int64_t tid;
    int fd;
    char *procname; (void) procname;

    tid = get_context_tid(event);

    procname = get_context_comm(event);

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);

    QHash<int, IOStateMachine> *stateMachines = (QHash<int, IOStateMachine>*)privateData;
    if (!stateMachines->contains(tid)) {
        IOStateMachine fsm;
        fsm.data.tid = tid;
        stateMachines->insert(tid, fsm);
    }
    IOStateMachine &fsm = (*stateMachines)[tid];
    IOStateMachine::EventParams<IOEvents::READ> p;

    fd = bt_ctf_get_uint64(bt_ctf_get_field(event,
                scope, "_fd"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing some context info\n");
        goto error;
    }

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        goto error;

    cpu_id = get_cpu_id(event);

    p.fd = fd;
    p.timestamp = timestamp;
    fsm.doTransition(IOEvents::READ, p);


    return BT_CB_OK;

error:
    return BT_CB_ERROR_STOP;
}

enum bt_cb_ret handleSysWriteWrapper(bt_ctf_event *event, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id; (void) cpu_id;
    int64_t tid;
    int fd;
    char *procname; (void) procname;

    tid = get_context_tid(event);

    procname = get_context_comm(event);

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);

    QHash<int, IOStateMachine> *stateMachines = (QHash<int, IOStateMachine>*)privateData;
    if (!stateMachines->contains(tid)) {
        IOStateMachine fsm;
        fsm.data.tid = tid;
        stateMachines->insert(tid, fsm);
    }
    IOStateMachine &fsm = (*stateMachines)[tid];
    IOStateMachine::EventParams<IOEvents::WRITE> p;

    fd = bt_ctf_get_uint64(bt_ctf_get_field(event,
                scope, "_fd"));
    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing some context info\n");
        goto error;
    }

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        goto error;

    cpu_id = get_cpu_id(event);

    p.fd = fd;
    p.timestamp = timestamp;
    fsm.doTransition(IOEvents::WRITE, p);

    return BT_CB_OK;

error:
    return BT_CB_ERROR_STOP;
}

enum bt_cb_ret handleExitSyscallWrapper(bt_ctf_event *event, void *privateData)
{
    const struct bt_definition *scope;
    uint64_t timestamp;
    uint64_t cpu_id; (void) cpu_id;
    int64_t tid;
    int64_t ret;
    char *procname; (void) procname;

    tid = get_context_tid(event);

    procname = get_context_comm(event);

    scope = bt_ctf_get_top_level_scope(event,
            BT_EVENT_FIELDS);

    ret = bt_ctf_get_int64(bt_ctf_get_field(event,
                    scope, "_ret"));

    QHash<int, IOStateMachine> *stateMachines = (QHash<int, IOStateMachine>*)privateData;
    if (!stateMachines->contains(tid)) {
        IOStateMachine fsm;
        fsm.data.tid = tid;
        stateMachines->insert(tid, fsm);
    }
    IOStateMachine &fsm = (*stateMachines)[tid];
    IOStateMachine::EventParams<IOEvents::EXIT> p;

    if (bt_ctf_field_get_error()) {
        fprintf(stderr, "Missing some context info\n");
        goto error;
    }

    timestamp = bt_ctf_get_timestamp(event);
    if (timestamp == -1ULL)
        goto error;

    cpu_id = get_cpu_id(event);

    p.ret = ret;
    p.timestamp = timestamp;
    fsm.doTransition(IOEvents::EXIT, p);

    return BT_CB_OK;

error:
    return BT_CB_ERROR_STOP;
}

void ParallelStateMachineTest::benchmarkSerialStateMachine()
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

    const QList<IOStateMachine> &machines = stateMachines.values();
    std::sort(machines.begin(), machines.end(), [](IOStateMachine a, IOStateMachine b) {
       return a.data.readCount > b.data.readCount;
    });
    for (auto i = machines.begin(); i != machines.end(); i++) {
            double avgReadLatency = i->data.readCount ? i->data.totalReadLatency / i->data.readCount : 0;
            qDebug() << i->data.tid << avgReadLatency << i->data.readCount;
    }

    bt_ctf_iter_destroy(iter);
    bt_context_put(ctx);
    int elapsed = timer.elapsed();
    qDebug() << "Elapsed for serial : " << elapsed << "ms.";
}

void ParallelStateMachineTest::benchmarkParallelStateMachine()
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

    //    // Instantiate functor and thread local storage for trace wrappers
    //    tbb::enumerable_thread_specific<TraceWrapper> tls([path]{ return TraceWrapper(path); });
    //    MapPass mapPass(tls);

        // Launch map reduce
        auto firstPassFuture = QtConcurrent::map(paramsList.begin(), paramsList.end(), doFirstPassMap);

        firstPassFuture.waitForFinished();

        for (size_t i = 0; i < paramsList.size() - 1; i++) {
            IOStateMachineMap &map = paramsList[i].stateMachines;
            IOStateMachineMap &nextMap = paramsList[i+1].stateMachines;
            for (auto kvp = map.begin(); kvp != map.end(); kvp++) {
                IOStateMachine &machine = kvp.value();
                machine.setEnumerativePass(false);
                if (nextMap.contains(machine.data.tid)) {
                    IOStateMachine &nextMachine = nextMap[machine.data.tid];
                    nextMachine.setCurrentState(machine.getCurrentStateForInitialState(machine.getCurrentState()));
                }
            }
        }
        IOStateMachineMap &last = paramsList.back().stateMachines;
        for (auto kvp = last.begin(); kvp != last.end(); kvp++) {
            IOStateMachine &machine = kvp.value();
            machine.setEnumerativePass(false);
        }

        auto secondPassFuture = QtConcurrent::mappedReduced(paramsList.begin(),
                                                            paramsList.end(),
                                                            doFirstPassMap, doSecondPassReduce,
                                                            QtConcurrent::OrderedReduce);

        QHash<int, IOData> finalResults = secondPassFuture.result();

        const auto &values = finalResults.values();
        std::sort(values.begin(), values.end(), [](const IOData &a, const IOData &b) {
           return a.readCount > b.readCount;
        });
        for (auto i = values.begin(); i != values.end(); i++) {
            double avgReadLatency = i->readCount ? i->totalReadLatency / i->readCount : 0;
            qDebug() << i->tid << avgReadLatency << i->readCount;
        }

        int elapsed = timer.elapsed();
        qDebug() << "Elapsed for parallel" << threads <<": " << elapsed << "ms.";
    }
}

IOStateMachineMap doFirstPassMap(MapParams &params)
{
    TraceWrapper &wrapper = params.wrapper;
    IOStateMachineMap &stateMachines = params.stateMachines;
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

    return params.stateMachines;
}

void doSecondPassReduce(QHash<int, IOData> &final, const IOStateMachineMap &map) {
    const QList<IOStateMachine> machines = map.values();
    for (auto machine = machines.begin(); machine != machines.end(); machine++) {
        const IOData &data = machine->data;
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
        if (!finalData.currentSyscall.name.empty()) {
            if (!data.unknownSyscall.name.empty()) {
                std::cout << data.tid << ":" <<
                    finalData.currentSyscall.start << "-" <<
                    data.unknownSyscall.end << " (" <<
                    finalData.currentSyscall.name << "/" <<
                    data.unknownSyscall.name << ")" << std::endl;
                uint64_t latency = data.unknownSyscall.end - finalData.currentSyscall.start;
                if (finalData.currentSyscall.name == "sys_read") {
                    finalData.totalReadLatency += latency;
                    finalData.readCount++;
                }
                if (finalData.currentSyscall.name == "sys_write") {
                    finalData.totalWriteLatency += latency;
                    finalData.writeCount++;
                }
            }
        }
        finalData.currentSyscall = data.currentSyscall;
    }
}

QTEST_APPLESS_MAIN(ParallelStateMachineTest)

#include "tst_parallelstatemachinetest.moc"

