#ifndef SCHED_H
#define SCHED_H

#include <QVector>
#include <QHash>

#include <babeltrace/ctf/events.h>
#include "common.h"

class Sched
{
public:
    Sched();
    void doSwitch(uint64_t timestamp, int64_t cpu, int prev_pid,
                  int next_pid, char *prev_comm, char *next_comm, char *hostname);

    QVector<Cpu>& getCpus();
    QHash<int, Process> &getTids();
    const QVector<Cpu>& getCpus() const;
    const QHash<int, Process> &getTids() const;
    uint64_t getStart() const;
    void setStart(uint64_t start);
    uint64_t getEnd() const;
    void setEnd(uint64_t end);

private:
    QVector<Cpu> cpus;
    QHash<int, Process> tids;
    uint64_t start;
    uint64_t end;

    Cpu &getCpu(unsigned int cpu);
};

#endif // SCHED_H
