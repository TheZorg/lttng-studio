#include "sched.h"

Sched::Sched() : cpus(), tids(), start(0), end(0)
{
}

void Sched::doSwitch(uint64_t timestamp, int64_t cpu, int prev_pid, int next_pid,
                     char *prev_comm, char *next_comm, char *hostname)
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

QVector<Cpu> &Sched::getCpus()
{
    return this->cpus;
}

QHash<int, Process> &Sched::getTids()
{
    return this->tids;
}

const QVector<Cpu> &Sched::getCpus() const
{
    return this->cpus;
}

const QHash<int, Process> &Sched::getTids() const
{
    return this->tids;
}

uint64_t Sched::getStart() const
{
    return this->start;
}

void Sched::setStart(uint64_t start)
{
    this->start = start;
}

uint64_t Sched::getEnd() const
{
    return this->end;
}

void Sched::setEnd(uint64_t end)
{
    this->end = end;
}

Cpu& Sched::getCpu(unsigned int cpu)
{
    QMutableVectorIterator<Cpu> iter(cpus);
    while (iter.hasNext()) {
        Cpu &tmp = iter.next();
        if (tmp.id == cpu) {
            return tmp;
        }
    }
    cpus.append(Cpu(cpu));
    return cpus.last();
}

