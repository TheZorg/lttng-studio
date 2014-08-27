#include <QString>
#include <QtTest>
#include <QDir>
#include <QProcessEnvironment>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <iostream>
#include <cstdio>

#include <babeltrace/babeltrace.h>
#include <babeltrace/format.h>
#include <babeltrace/context.h>
#include <babeltrace/iterator.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/ctf/iterator.h>

using namespace std;

class BabeltraceTest : public QObject
{
    Q_OBJECT
    
public:
    BabeltraceTest();
    
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testEventCount();
    void testMultiIter();
private:
    QDir traceDir;
};

BabeltraceTest::BabeltraceTest()
{
}

void BabeltraceTest::initTestCase()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList path = QStringList() << env.value("top_srcdir") << "3rdparty"
                                     << "babeltrace" << "tests" << "ctf-traces";
    traceDir.setPath(path.join(QDir::separator()));
}

void BabeltraceTest::cleanupTestCase()
{
}

int countEvents(bt_ctf_iter *iter)
{
    struct bt_ctf_event *ctf_event;
    int count = 0;


    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        count++;
        bt_iter_next(bt_ctf_get_iter(iter));
    }

    return count;
}

void BabeltraceTest::testMultiIter()
{
    bt_ctf_iter *iter1, *iter2;
    int count1, count2;
    count1 = count2 = 0;
    struct bt_iter_pos begin_pos;

    QString path = traceDir.absolutePath() + QDir::separator() + "succeed" +
            QDir::separator() + "lttng-modules-2.0-pre5";

    // open a trace
    struct bt_context *ctx1 = bt_context_create();
    int trace_id = bt_context_add_trace(ctx1, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    QVERIFY2(trace_id >= 0, "Failed: bt_context_add_trace");
    begin_pos.type = BT_SEEK_BEGIN;
    iter1 = bt_ctf_iter_create(ctx1, &begin_pos, NULL);

    struct bt_context *ctx2 = bt_context_create();
    trace_id = bt_context_add_trace(ctx2, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    QVERIFY2(trace_id >= 0, "Failed: bt_context_add_trace");
    begin_pos.type = BT_SEEK_BEGIN;
    iter2 = bt_ctf_iter_create(ctx2, &begin_pos, NULL);

    QFuture<int> countFuture1 = QtConcurrent::run(countEvents, iter1);
    QFuture<int> countFuture2 = QtConcurrent::run(countEvents, iter2);

    count1 = countFuture1.result();
    count2 = countFuture2.result();

    bt_context_put(ctx1);
    bt_context_put(ctx2);
    QVERIFY2(count1 == count2, "Wrong event count");
}

void BabeltraceTest::testEventCount()
{
    struct bt_ctf_iter *iter;
    struct bt_iter_pos begin_pos;
    struct bt_ctf_event *ctf_event;
    int count = 0;

    QString path = traceDir.absolutePath() + QDir::separator() + "succeed" +
            QDir::separator() + "wk-heartbeat-u";

    // open a trace
    struct bt_context *ctx = bt_context_create();
    int trace_id = bt_context_add_trace(ctx, path.toStdString().c_str(), "ctf", NULL, NULL, NULL);
    QVERIFY2(trace_id >= 0, "Failed: bt_context_add_trace");

    // read all event
    begin_pos.type = BT_SEEK_BEGIN;
    iter = bt_ctf_iter_create(ctx, &begin_pos, NULL);
    while((ctf_event = bt_ctf_iter_read_event(iter))) {
        count++;
        bt_iter_next(bt_ctf_get_iter(iter));
    }
    bt_context_put(ctx);
    QVERIFY2(count == 20, "Wrong event count");
}

QTEST_APPLESS_MAIN(BabeltraceTest)

#include "tst_babeltracetest.moc"
