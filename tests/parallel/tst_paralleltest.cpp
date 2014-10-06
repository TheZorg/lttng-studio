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

extern "C" {
#include <babeltrace/babeltrace.h>
#include <babeltrace/format.h>
#include <babeltrace/context.h>
#include <babeltrace/iterator.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/ctf/iterator.h>
#include "babeltrace/ctf/types.h"
}

using namespace std;

// Number of chunks the trace will be split into by default
static const int DEFAULT_SPLITS = 8;

class ParallelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    /*!
     * \brief Serial implementation.
     *
     * Baseline implementation when analysing speedup.
     */
    void benchmarkSerial();

    /*!
     * \brief Unbalanced implementation.
     *
     * Splits the trace according to timestamp, in a
     * predefined number of chunks.
     * \see DEFAULT_SPLITS
     */
    void benchmarkParallelUnbalanced();

    /*!
     * \brief Balanced implementation.
     *
     * Uses the packet index to balance the load on
     * the different threads.
     * NOTE: ONLY WORKS FOR SINGLE STREAM AT THE MOMENT.
     */
    void benchmarkParallelBalanced();

    /*!
     * \brief Per-stream implementation.
     *
     * Splits the trace by assigning one stream per
     * thread.
     */
    void benchmarkParallelPerStream();

    /*!
     * \brief TBB implementation
     *
     * Uses TBB to split the trace and dispatch work
     * to threads.
     */
    void benchmarkTbb();
private:
    QDir traceDir;
    QDir perStreamTraceDir;

private:
    int countSerial();
    int countParallel();
    int countParallelMulti();
    int countParallelMapReduce(int splits);
    int countParallelMapReduceBalanced();
    int countParallelTbb(uint64_t minChunk);
    int countParallelPerStream();
};

/*!
 * \brief The map_params struct is used to pass
 * arguments to the map-reduce functions.
 */
struct map_params {
    QString tracePath;
    struct bt_iter_pos begin_pos;
    struct bt_iter_pos end_pos;
};

/*!
 * \brief Callback for packet seeking.
 *
 * Workaround way to get to the packet index to
 * balance the load.
 */
void seek(bt_stream_pos *pos, size_t index, int whence);

/*!
 * \brief Event counting routine. Used by mappedReduced.
 * \param params Parameters used to identify trace, time range, etc.
 * \return Number of events.
 */
int doCount(struct map_params params);

/*!
 * \brief Event summing routine. Used by mappedReduced.
 * \param finalResult Accumulator for final count.
 * \param intermediate Count to add to accumulator.
 */
void doSum(int &finalResult, const int &intermediate);

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
    TraceWrapper(TraceWrapper &&other) : ctx(std::move(other.ctx)) { }

    ~TraceWrapper() {
        bt_context_put(ctx);
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
                qDebug() << "Failed: bt_context_add_trace";
            }
        }
        return ctx;
    }
};

/*!
 * \brief The EventCountTbb struct is the functor used
 * by TBB for event counting and summing.
 */
struct EventCountTbb {
    int mySum;
    tbb::enumerable_thread_specific<TraceWrapper> &myTLS; // Thread local storage
    QString myTracePath;

    EventCountTbb(tbb::enumerable_thread_specific<TraceWrapper> &tls) : mySum(0), myTLS(tls) {}
    EventCountTbb(EventCountTbb &other, tbb::split) : EventCountTbb(other.myTLS) {}

    void operator()(const tbb::blocked_range<uint64_t> &r) {
        uint64_t begin = r.begin();
        uint64_t end = r.end();
        TraceWrapper &wrapper = myTLS.local();
        bt_context *myCtx = wrapper.getContext();
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

        qDebug() << "Counted" << sum - mySum << "events";

        bt_ctf_iter_destroy(iter);

        mySum = sum;
    }

    void join (const EventCountTbb &other) {
        mySum += other.mySum;
    }
};

/*!
 * \brief The EventCount struct is the functor object used
 * by the map-reduce functions.
 *
 * It is necessary to reuse the functor so that context creation
 * costs are amortized with trace size.
 */
struct EventCount
{
    typedef int result_type;
    tbb::enumerable_thread_specific<TraceWrapper> &myTLS; // Thread local storage

    EventCount(tbb::enumerable_thread_specific<TraceWrapper> &tls) : myTLS(tls) { }

    EventCount(const EventCount &other) : EventCount(other.myTLS) { }

    int operator()(const struct map_params params) {
        TraceWrapper &wrapper = myTLS.local();
        bt_context *myCtx = wrapper.getContext();
        int sum = 0;
        bt_ctf_event *ctf_event;

        struct bt_ctf_iter *iter = bt_ctf_iter_create(myCtx, &params.begin_pos, &params.end_pos);

        while((ctf_event = bt_ctf_iter_read_event(iter))) {
            sum++;
            bt_iter_next(bt_ctf_get_iter(iter));
        }

        qDebug() << "Counted" << sum << "events";

        bt_ctf_iter_destroy(iter);
        return sum;
    }
};

void ParallelTest::initTestCase()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
//    QStringList path = QStringList() << env.value("top_srcdir") << "3rdparty"
//                                     << "babeltrace" << "tests" << "ctf-traces"
//                                     << "succeed" << "lttng-modules-2.0-pre5";
    QStringList path = QStringList() << "/home" << "fabien" << "lttng-traces"
                                        << "unbalanced-20140916-143242";
//                                        << "test-20140619-171512";
//                                        << "sinoscope-20140708-150630";
//                                        << "live-test-20140805-153033";
    traceDir.setPath(path.join(QDir::separator()) + "/kernel_per_stream/channel0_0.d");
    perStreamTraceDir.setPath(path.join(QDir::separator()) + "/kernel_per_stream");
}

void ParallelTest::benchmarkSerial()
{
    volatile int count;
    QTime timer;
    timer.start();
    count = countSerial();
    int milliseconds = timer.elapsed();
    qDebug() << "Event count : " << count;
    qDebug() << "Serial Elapsed : " << milliseconds << "ms";
}

void ParallelTest::benchmarkParallelUnbalanced()
{
    volatile int count;
    QTime timer;
    timer.start();
    count = countParallelMapReduce(DEFAULT_SPLITS);
    int milliseconds = timer.elapsed();
    qDebug() << "Event count : " << count;
    qDebug() << "Unbalanced Elapsed : " << milliseconds << "ms";
}

void ParallelTest::benchmarkParallelPerStream()
{
    volatile int count;
    QTime timer;
    timer.start();
    count = countParallelPerStream();
    int milliseconds = timer.elapsed();
    qDebug() << "Event count : " << count;
    qDebug() << "Per Stream Elapsed : " << milliseconds << "ms";
}

void ParallelTest::benchmarkParallelBalanced()
{
    volatile int count;
    QTime timer;
    timer.start();
    count = countParallelMapReduceBalanced();
    int milliseconds = timer.elapsed();
    qDebug() << "Event count : " << count;
    qDebug() << "Balanced Elapsed : " << milliseconds << "ms";
}

void ParallelTest::benchmarkTbb()
{
    volatile int count;
    QTime timer;
    timer.start();
    count = countParallelTbb(1000000000);
    int milliseconds = timer.elapsed();
    qDebug() << "Event count : " << count;
    qDebug() << "TBB Elapsed : " << milliseconds << "ms";
}

int ParallelTest::countSerial()
{
    int count = 0;
    QString path = traceDir.absolutePath();

    struct map_params params;
    params.tracePath = path;
    params.begin_pos.type = BT_SEEK_BEGIN;
    params.end_pos.type = BT_SEEK_LAST;

    count = doCount(params);

    return count;
}

int ParallelTest::countParallelMapReduce(int splits)
{
    struct bt_iter_pos positions[splits+1];
    QList< struct map_params > paramsList;
    struct bt_iter_pos end_pos;

    QString path = traceDir.absolutePath();

    // Open a trace to get the begin/end timestamps
    struct bt_context *ctx = bt_context_create();
    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
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
    uint64_t step = (end - begin)/splits;

    positions[0].type = BT_SEEK_BEGIN;
    for (int i = 1; i < splits; i++)
    {
        positions[i].type = BT_SEEK_TIME;
        positions[i].u.seek_time = begin + (i*step);
    }
    positions[splits].type = BT_SEEK_LAST;

    // Build the params list
    for (int i = 0; i < splits; i++)
    {
        struct map_params params;
        params.tracePath = path;
        params.begin_pos = positions[i];
        params.end_pos = positions[i+1];
        paramsList << params;
    }

    // Instantiate functor and thread local storage for trace wrappers
    tbb::enumerable_thread_specific<TraceWrapper> tls([path]{ return TraceWrapper(path); });
    EventCount ec(tls);

    // Launch map reduce
    QFuture<int> countFuture = QtConcurrent::mappedReduced(paramsList, ec, doSum);

    int count = countFuture.result();

    return count;
}

int ParallelTest::countParallelTbb(uint64_t minChunk)
{
    struct bt_iter_pos end_pos;

    QString path = traceDir.absolutePath();

    // Open a trace to get the begin/end timestamps
    struct bt_context *ctx = bt_context_create();
    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
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

    // Instantiate functor and thread local storage for trace wrappers
    tbb::enumerable_thread_specific<TraceWrapper> tls([path]{ return TraceWrapper(path); });
    EventCountTbb ec(tls);
    tbb::affinity_partitioner ap;

    // Launch map reduce
    tbb::parallel_reduce(tbb::blocked_range<uint64_t>(begin, end, minChunk), ec, ap);

    int count = ec.mySum;

    return count;
}

int ParallelTest::countParallelPerStream()
{
    QList< struct map_params > paramsList;

    // Retrieve all the split trace directories
    QFileInfoList fileInfoList = perStreamTraceDir.entryInfoList(QStringList() << "*.d", QDir::Dirs);

    // Build the params list
    for (int i = 0; i < fileInfoList.size(); i++)
    {
        QFileInfo fileInfo = fileInfoList.at(i);
        struct map_params params;
        params.tracePath = fileInfo.absoluteFilePath();
        params.begin_pos.type = BT_SEEK_BEGIN;
        params.end_pos.type = BT_SEEK_LAST;
        paramsList << params;
    }

    // TODO: use the functor
//    tbb::enumerable_thread_specific<TraceWrapper> tls([path]{ return TraceWrapper(path); });
//    EventCount ec(tls);

    // Launch map reduce
    QFuture<int> countFuture = QtConcurrent::mappedReduced(paramsList, doCount, doSum);

    int count = countFuture.result();
    return count;
}

// Holds the balanced chunk begin/end timestamps
QVector<bt_iter_pos> positions;

int ParallelTest::countParallelMapReduceBalanced()
{
    QList< struct map_params > paramsList;
    positions.clear();

    // TODO: make it work with multiple streams
    QString path = perStreamTraceDir.absolutePath() + "/channel0_0.d";

    // open a trace
    struct bt_context *ctx = bt_context_create();
    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", seek, NULL, NULL);
    if(trace_id < 0)
    {
        qDebug() << "Failed: bt_context_add_trace";
        return 0;
    }

    // Read a single event to trigger the packet_seek function
    bt_ctf_iter *iter = bt_ctf_iter_create(ctx, NULL, NULL);
    bt_ctf_iter_read_event(iter);

    // We don't need that context anymore, dispose of it
    bt_ctf_iter_destroy(iter);
    bt_context_put(ctx);

    // Build the params list;
    for (int i = 0; i < positions.size() - 1; i++)
    {
        struct map_params params;
        params.tracePath = path;
        params.begin_pos = positions[i];
        params.end_pos = positions[i+1];
        params.begin_pos.u.seek_time += 1; // Fixes timestamp overlap issues
        paramsList << params;
    }

    // Instantiate functor and thread local storage for trace wrappers
    tbb::enumerable_thread_specific<TraceWrapper> tls([path]{ return TraceWrapper(path); });
    EventCount ec(tls);

    // Launch map reduce
    QFuture<int> countFuture = QtConcurrent::mappedReduced(paramsList, ec, doSum);

    int count = countFuture.result();
    return count;
}

void seek(bt_stream_pos *pos, size_t index, int whence)
{
    // Get the packet index from the current position
    struct ctf_stream_pos* p = ctf_pos(pos);
    struct packet_index *idx;
    unsigned int num_packets = p->packet_index->len;

    // Get the total size for all the packets
    uint64_t size = 0;
    for (unsigned int i = 0; i < num_packets; i++) {
        idx = &g_array_index(p->packet_index, struct packet_index, i);
        size += idx->content_size;
    }

    // Try to split the packets "evenly"
    uint64_t maxPacketSize = size/num_packets;

    qDebug() << "Num packets:" << num_packets;

    // Accumulator for packet size
    uint64_t acc = 0;

    // Add start of trace to positions
    bt_iter_pos iter_pos;
    iter_pos.type = BT_SEEK_BEGIN;
    positions << iter_pos;

    iter_pos.type = BT_SEEK_TIME;
    // Iterate through packets, accumulating until we have a big enough chunk
    // NOTE: disregard last packet, since the BT_SEEK_LAST will take care of it
    for (unsigned int i = 0; i < num_packets - 1; i++) {
        idx = &g_array_index(p->packet_index, struct packet_index, i);
        if (acc >= maxPacketSize) {
            // Our chunk is big enough, add the timestamp to our positions
            acc = 0;
            iter_pos.u.seek_time = idx->ts_real.timestamp_end;
            positions << iter_pos;
        }
        acc += idx->content_size;
    }
    // Add end of trace to positions
    iter_pos.type = BT_SEEK_LAST;
    positions << iter_pos;
    qDebug() << "Using " << positions.size() - 1 << " chunks";

    // Do the actual seek
    ctf_packet_seek(pos, index, whence);
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

    qDebug() << "Counted" << count << "events";

    return count;
}

void doSum(int &finalResult, const int &intermediate)
{
    finalResult += intermediate;
}

QTEST_APPLESS_MAIN(ParallelTest)

#include "tst_paralleltest.moc"
