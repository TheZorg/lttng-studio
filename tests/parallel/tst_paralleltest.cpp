#include <QString>
#include <QtTest>
#include <QDir>
#include <QProcessEnvironment>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <tbb/tbb.h>

#include <iostream>
#include <cstdio>

#include <stdint.h>

#include <babeltrace/babeltrace.h>
#include <babeltrace/format.h>
#include <babeltrace/context.h>
#include <babeltrace/iterator.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/ctf/iterator.h>

using namespace std;

int NUM_THREADS = 8;

class ParallelTest : public QObject
{
    Q_OBJECT

public:
    ParallelTest();

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void benchmarkSerial();
    void benchmarkParallel();
private:
    QDir traceDir;
private:
    int countSerial();
    int countParallel();
    int countParallelMulti();
    int countParallelMapReduce();
    int countParallelMapReduceBalanced();
    int countParallelTbb(uint64_t minChunk);
};

struct map_params {
    QString tracePath;
    struct bt_iter_pos begin_pos;
    struct bt_iter_pos end_pos;
};

int doCount(struct map_params params);
void doSum(int &finalResult, const int &intermediate);

ParallelTest::ParallelTest()
{
}

void ParallelTest::initTestCase()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
//    QStringList path = QStringList() << env.value("top_srcdir") << "3rdparty"
//                                     << "babeltrace" << "tests" << "ctf-traces"
//                                     << "succeed" << "lttng-modules-2.0-pre5";
    QStringList path = QStringList() << "/home" << "fabien" << "lttng-traces"
                                        << "live-test-20140805-153033" << "kernel";
    traceDir.setPath(path.join(QDir::separator()));
}

void ParallelTest::cleanupTestCase()
{
}

void ParallelTest::benchmarkSerial()
{
    return;
    volatile int count;
    QTime timer;
    timer.start();
    count = countSerial();
    int milliseconds = timer.elapsed();
    qDebug() << "Event count : " << count;
    qDebug() << "Elapsed : " << milliseconds << "ms";
}

void ParallelTest::benchmarkParallel()
{
//    return;
    volatile int count;
    QTime timer;
    uint64_t minChunks[] = { 1000000000, /*2000000000, 4000000000, 8000000000*/ };
    for (int i = 0; i < sizeof(minChunks)/sizeof(uint64_t); i++) {
        timer.restart();
        count = countParallelTbb(minChunks[i]);
        int milliseconds = timer.elapsed();
        qDebug() << "Min chunk : " << minChunks[i];
        qDebug() << "Event count : " << count;
        qDebug() << "Elapsed : " << milliseconds << "ms";
    }
//    timer.restart();
//    count = countParallelMapReduce();
//    int milliseconds = timer.elapsed();
//    qDebug() << "Event count : " << count;
//    qDebug() << "Elapsed : " << milliseconds << "ms";
}

int ParallelTest::countSerial()
{
    QTime timer;
    timer.start();
    struct bt_ctf_iter *iter;
    struct bt_iter_pos begin_pos;
    struct bt_ctf_event *ctf_event;
    int count = 0;

    QString path = traceDir.absolutePath();

    // open a trace
//    struct bt_context *ctx = bt_context_create();
//    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);

//    if(trace_id < 0)
//    {
//        qDebug() << "Failed: bt_context_add_trace";
//        return 0;
//    }

//    // read all event
//    begin_pos.type = BT_SEEK_BEGIN;
//    iter = bt_ctf_iter_create(ctx, &begin_pos, NULL);

    struct map_params params;
    params.tracePath = path;
    params.begin_pos.type = BT_SEEK_BEGIN;
    params.end_pos.type = BT_SEEK_LAST;

    int milliseconds = timer.elapsed();
    qDebug() << "Elapsed for init : " << milliseconds << "ms";
    timer.restart();

    count = doCount(params);

    milliseconds = timer.elapsed();
    qDebug() << "Elapsed for map reduce : " << milliseconds << "ms";

//    bt_context_put(ctx);
    return count;
}
/*
int ParallelTest::countParallel()
{
    bt_ctf_iter *iter1, *iter2;
    int count1, count2;
    count1 = count2 = 0;
    struct bt_iter_pos begin_pos, middle_pos, end_pos;
    struct bt_ctf_event *event;

    QString path = traceDir.absolutePath();

    // open a trace
    struct bt_context *ctx1 = bt_context_create();
    int trace_id = bt_context_add_trace(ctx1, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
    }

    begin_pos.type = BT_SEEK_BEGIN;
    end_pos.type = BT_SEEK_LAST;

    // Get begin timestamp
    iter1 = bt_ctf_iter_create(ctx1, NULL, NULL);
    event = bt_ctf_iter_read_event(iter1);
    uint64_t begin = bt_ctf_get_timestamp(event);

    // Get end timestamp
    bt_iter_set_pos(bt_ctf_get_iter(iter1), &end_pos);
    event = bt_ctf_iter_read_event(iter1);
    uint64_t end = bt_ctf_get_timestamp(event);

    // Calc middle timestamp
    uint64_t middle = begin + ((end - begin)/2);

//    qDebug() << "Begin  : " << begin;
//    qDebug() << "Middle : " << middle;
//    qDebug() << "End    : " << end;

    // Reset iterator
    bt_ctf_iter_destroy(iter1);
    middle_pos.type = begin_pos.type = end_pos.type =BT_SEEK_TIME;
    begin_pos.u.seek_time = begin;
    middle_pos.u.seek_time = middle;
    end_pos.u.seek_time = end;
    iter1 = bt_ctf_iter_create(ctx1, &begin_pos, &middle_pos);

    struct bt_context *ctx2 = bt_context_create();
    trace_id = bt_context_add_trace(ctx2, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
    }
    iter2 = bt_ctf_iter_create(ctx2, &middle_pos, &end_pos);

    QFuture<int> countFuture1 = QtConcurrent::run(doCount, iter1);
    QFuture<int> countFuture2 = QtConcurrent::run(doCount, iter2);

    count1 = countFuture1.result();
    count2 = countFuture2.result();

    qDebug() << "Count 1 : " << count1 << ", Count 2 : " << count2;
    bt_context_put(ctx1);
    bt_context_put(ctx2);
    return count1 + count2;
}

int ParallelTest::countParallelMulti()
{
    struct bt_context *ctxs[NUM_THREADS];
    struct bt_iter_pos positions[NUM_THREADS+1];
    QList< QFuture<int> > countFutures;
    struct bt_iter_pos begin_pos, end_pos;

    QString path = traceDir.absolutePath();

    // open a trace
    struct bt_context *ctx = bt_context_create();
    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
    }

    begin_pos.type = BT_SEEK_BEGIN;
    end_pos.type = BT_SEEK_LAST;

    // Get begin timestamp
    struct bt_ctf_iter* iter = bt_ctf_iter_create(ctx, NULL, NULL);
    struct bt_ctf_event *event = bt_ctf_iter_read_event(iter);
    uint64_t begin = bt_ctf_get_timestamp(event);

    // Get end timestamp
    bt_iter_set_pos(bt_ctf_get_iter(iter), &end_pos);
    event = bt_ctf_iter_read_event(iter);
    uint64_t end = bt_ctf_get_timestamp(event);

    bt_context_put(ctx);

    uint64_t step = (end - begin)/NUM_THREADS;

    positions[0].type = BT_SEEK_BEGIN;
    for (int i = 1; i < NUM_THREADS; i++)
    {
        positions[i].type = BT_SEEK_TIME;
        positions[i].u.seek_time = begin + (i*step);
    }
    positions[NUM_THREADS].type = BT_SEEK_LAST;

    for (int i = 0; i < NUM_THREADS; i++)
    {
        ctxs[i] = bt_context_create();
        trace_id = bt_context_add_trace(ctxs[i], path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
        if(trace_id < 0)
        {
            qDebug() << "Failed: bt_context_add_trace";
            return 0;
        }

        struct bt_ctf_iter *thread_iter = bt_ctf_iter_create(ctxs[i], &positions[i], &positions[i+1]);
        countFutures << QtConcurrent::run(doCount, thread_iter);
    }

    int acc = 0;
    while (!countFutures.isEmpty())
    {
        acc += countFutures.takeFirst().result();
    }

    for (int i = 0; i < NUM_THREADS; i++)
    {
        bt_context_put(ctxs[i]);
    }
    return acc;
}
*/
int ParallelTest::countParallelMapReduce()
{
//    QTime timer;
//    timer.start();
    struct bt_context *ctxs[NUM_THREADS];
    struct bt_iter_pos positions[NUM_THREADS+1];
    QList< struct map_params > paramsList;
    struct bt_iter_pos begin_pos, end_pos;

    QString path = traceDir.absolutePath();

    // open a trace
    struct bt_context *ctx = bt_context_create();
    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
    }

    begin_pos.type = BT_SEEK_BEGIN;
    end_pos.type = BT_SEEK_LAST;

    // Get begin timestamp
    struct bt_ctf_iter* iter = bt_ctf_iter_create(ctx, NULL, NULL);
    struct bt_ctf_event *event = bt_ctf_iter_read_event(iter);
    uint64_t begin = bt_ctf_get_timestamp(event);

    // Get end timestamp
    bt_iter_set_pos(bt_ctf_get_iter(iter), &end_pos);
    event = bt_ctf_iter_read_event(iter);
    uint64_t end = bt_ctf_get_timestamp(event);

    bt_context_put(ctx);

    uint64_t step = (end - begin)/NUM_THREADS;

    positions[0].type = BT_SEEK_BEGIN;
    for (int i = 1; i < NUM_THREADS; i++)
    {
        positions[i].type = BT_SEEK_TIME;
        positions[i].u.seek_time = begin + (i*step);
    }
    positions[NUM_THREADS].type = BT_SEEK_LAST;

    for (int i = 0; i < NUM_THREADS; i++)
    {
        struct map_params params;
        params.tracePath = path;
        params.begin_pos = positions[i];
        params.end_pos = positions[i+1];
        paramsList << params;
    }

//    int milliseconds = timer.elapsed();
//    qDebug() << "Elapsed for init : " << milliseconds << "ms";
//    timer.restart();

    QFuture<int> countFuture = QtConcurrent::mappedReduced(paramsList, doCount, doSum);

    int count = countFuture.result();

//    milliseconds = timer.elapsed();
//    qDebug() << "Elapsed for map reduce : " << milliseconds << "ms";

//    for (int i = 0; i < NUM_THREADS; i++)
//    {
//        bt_context_put(ctxs[i]);
//    }
    return count;
}

int ParallelTest::countParallelMapReduceBalanced()
{

}

struct TimestampRange {
    uint64_t begin;
    uint64_t end;
    uint64_t minChunk;
    bool empty() const { return begin == end; }
    bool is_divisible() const { return (end - begin) > minChunk; }
    TimestampRange(uint64_t begin, uint64_t end, uint64_t minChunk) :
        begin(begin), end(end), minChunk(minChunk) {}
    TimestampRange(TimestampRange &r, tbb::split) : minChunk(r.minChunk) {
        uint64_t m = (r.begin + r.end)/2;
        this->begin = m;
        this->end = r.end;
        r.end = m;
    }
};

struct TraceWrapper {
    bt_context *ctx;
    TraceWrapper(QString tracePath) {
//        qDebug() << "Making wrapper";
        ctx = bt_context_create();
        int trace_id = bt_context_add_trace(ctx, tracePath.toStdString().c_str(), "ctf", NULL, NULL, NULL);
        if(trace_id < 0)
        {
            qDebug() << "Failed: bt_context_add_trace";
        }
    }

    TraceWrapper(TraceWrapper &other) : ctx(other.ctx) {
//        qDebug() << "Copying wrapper";
        bt_context_get(ctx);
    }

    TraceWrapper(TraceWrapper &&other) : ctx(std::move(other.ctx)) {
//        qDebug() << "Moving wrapper";
    }

    ~TraceWrapper() {
//        qDebug() << "Destroying wrapper";
        bt_context_put(ctx);
    }
};

struct EventCount {
    int mySum;
    tbb::enumerable_thread_specific<TraceWrapper> &myTLS; // Thread local storage
    QString myTracePath;
    EventCount(tbb::enumerable_thread_specific<TraceWrapper> &tls) : mySum(0), myTLS(tls) {}
    EventCount(EventCount &other, tbb::split) : EventCount(other.myTLS) { }
    void operator()(const tbb::blocked_range<uint64_t> &r) {
        uint64_t begin = r.begin();
        uint64_t end = r.end();
//        qDebug() << "Calculating range between " << begin << " and " << end;
        TraceWrapper &wrapper = myTLS.local();
        bt_context *myCtx = wrapper.ctx;
        int sum = mySum;
        bt_iter_pos begin_pos, end_pos;
        bt_ctf_event *ctf_event;

        begin_pos.type = end_pos.type = BT_SEEK_TIME;
        begin_pos.u.seek_time = begin;
        end_pos.u.seek_time = end;

        struct bt_ctf_iter *iter = bt_ctf_iter_create(myCtx, &begin_pos, &end_pos);

        while((ctf_event = bt_ctf_iter_read_event(iter))) {
            sum++;
            bt_iter_next(bt_ctf_get_iter(iter));
        }

        bt_ctf_iter_destroy(iter);

        mySum = sum;
    }
    void join (const EventCount &other) { mySum += other.mySum; }
};

int ParallelTest::countParallelTbb(uint64_t minChunk)
{
    //    QTime timer;
    //    timer.start();
        struct bt_context *ctxs[NUM_THREADS];
        struct bt_iter_pos positions[NUM_THREADS+1];
        QList< struct map_params > paramsList;
        struct bt_iter_pos begin_pos, end_pos;

        QString path = traceDir.absolutePath();

        // open a trace
        struct bt_context *ctx = bt_context_create();
        int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
        if(trace_id < 0)
        {
            qDebug() << "Failed: bt_context_add_trace";
            return 0;
        }

        begin_pos.type = BT_SEEK_BEGIN;
        end_pos.type = BT_SEEK_LAST;

        // Get begin timestamp
        struct bt_ctf_iter* iter = bt_ctf_iter_create(ctx, NULL, NULL);
        struct bt_ctf_event *event = bt_ctf_iter_read_event(iter);
        uint64_t begin = bt_ctf_get_timestamp(event);

        // Get end timestamp
        bt_iter_set_pos(bt_ctf_get_iter(iter), &end_pos);
        event = bt_ctf_iter_read_event(iter);
        uint64_t end = bt_ctf_get_timestamp(event);

        bt_context_put(ctx);

        //    int milliseconds = timer.elapsed();
        //    qDebug() << "Elapsed for init : " << milliseconds << "ms";
        //    timer.restart();

        tbb::enumerable_thread_specific<TraceWrapper> tls([path]{ return TraceWrapper(path); });
        EventCount ec(tls);
        tbb::affinity_partitioner ap;
        tbb::parallel_reduce(tbb::blocked_range<uint64_t>(begin, end, minChunk), ec, ap);

//        milliseconds = timer.elapsed();
           //    qDebug() << "Elapsed for map reduce : " << milliseconds << "ms";

        int count = ec.mySum;

        return count;
}

int doCount(struct map_params params)
{
    struct bt_ctf_event *ctf_event;
    int count = 0;
    struct bt_context *ctx;
    int trace_id;

    ctx = bt_context_create();
    trace_id = bt_context_add_trace(ctx, params.tracePath.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
    }

    struct bt_ctf_iter *iter = bt_ctf_iter_create(ctx, &params.begin_pos, &params.end_pos);

    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        count++;
        bt_iter_next(bt_ctf_get_iter(iter));
    }

    bt_ctf_iter_destroy(iter);

    bt_context_put(ctx);

    return count;
}

void doSum(int &finalResult, const int &intermediate)
{
    finalResult += intermediate;
}

QTEST_APPLESS_MAIN(ParallelTest)

#include "tst_paralleltest.moc"
