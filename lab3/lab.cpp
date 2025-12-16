/* lab_os_fixed.cpp
   Variant 11: Optimistic set (optimistic synchronization) + MCSP (single producer, multiple consumers)
*/

#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <getopt.h>
#include <limits.h>
#include <random>
#include <sys/sysinfo.h>
using namespace std;

/* --------------------- Utility: time measurement --------------------- */

static double time_sec() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static inline void do_yield() {
    sched_yield();
}

/* --------------------- Optimistic set (no immediate delete) --------------------- */

struct ONode {
    int key;
    ONode* next;
    pthread_mutex_t lock;
    ONode(int k): key(k), next(nullptr) { pthread_mutex_init(&lock, nullptr); }
    ~ONode(){ pthread_mutex_destroy(&lock); }
};

class OptimisticSet {
private:
    ONode* head; // sentinel -INF
    ONode* tail; // sentinel +INF
public:
    OptimisticSet() {
        head = new ONode(INT_MIN);
        tail = new ONode(INT_MAX);
        head->next = tail;
    }
    ~OptimisticSet() {
        // Note: we don't delete nodes removed earlier (we also avoid deletes in remove)
        // But for destructor, traverse and delete nodes (safe because no concurrent access)
        ONode* cur = head;
        while(cur) {
            ONode* nxt = cur->next;
            delete cur;
            cur = nxt;
        }
    }

    void find(int key, ONode** out_pred, ONode** out_curr) {
        ONode* pred = head;
        ONode* curr = pred->next;
        while(curr->key < key) {
            pred = curr;
            curr = curr->next;
        }
        *out_pred = pred;
        *out_curr = curr;
    }

    bool validate(ONode* pred, ONode* curr) {
        ONode* p = head;
        while(p->key <= pred->key) {
            if(p == pred) return (pred->next == curr);
            p = p->next;
        }
        return false;
    }

    bool add(int key) {
        while(true) {
            ONode* pred; ONode* curr;
            find(key, &pred, &curr);
            pthread_mutex_lock(&pred->lock);
            pthread_mutex_lock(&curr->lock);
            if(!validate(pred, curr)) {
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&pred->lock);
                do_yield();
                continue;
            }
            if(curr->key == key) {
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&pred->lock);
                return false;
            } else {
                ONode* node = new ONode(key);
                node->next = curr;
                pred->next = node;
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&pred->lock);
                return true;
            }
        }
    }

    bool remove(int key) {
        while(true) {
            ONode* pred; ONode* curr;
            find(key, &pred, &curr);
            pthread_mutex_lock(&pred->lock);
            pthread_mutex_lock(&curr->lock);
            if(!validate(pred, curr)) {
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&pred->lock);
                do_yield();
                continue;
            }
            if(curr->key != key) {
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&pred->lock);
                return false;
            } else {
                // unlink node from list, but DO NOT delete memory here (avoid use-after-free)
                pred->next = curr->next;
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&pred->lock);
                // intentionally NOT delete curr;
                return true;
            }
        }
    }

    bool contains(int key) {
        ONode* curr = head;
        while(curr->key < key) curr = curr->next;
        return curr->key == key;
    }

    vector<int> snapshot() {
        vector<int> out;
        ONode* cur = head->next;
        while(cur && cur != tail) {
            out.push_back(cur->key);
            cur = cur->next;
        }
        return out;
    }
};

/* --------------------- MS-style queue (safe for MC, single producer) --------------------- */

class MCSPQueue {
private:
    int capacity;
    int *buf;        // values
    int *ready;      // flags: 0 = not ready, 1 = ready
    long producer_index;     // single producer (no need atomic)
    long consumer_index;     // atomic for multiple consumers

public:
    MCSPQueue(int cap): capacity(cap), producer_index(0), consumer_index(0) {
        buf = (int*)malloc(sizeof(int) * (size_t)capacity);
        ready = (int*)malloc(sizeof(int) * (size_t)capacity);
        if(!buf || !ready) { fprintf(stderr, "MCSPQueue: malloc failed\n"); exit(1); }
        for(int i=0;i<capacity;i++) ready[i] = 0;
    }
    ~MCSPQueue() {
        free(buf);
        free(ready);
    }

    // single producer
    void enqueue(int v) {
        long idx = producer_index++;
        if(idx >= capacity) {
            fprintf(stderr, "MCSPQueue::enqueue overflow idx=%ld cap=%d\n", idx, capacity);
            return;
        }
        buf[idx] = v;
        __sync_synchronize();
        __sync_lock_test_and_set(&ready[idx], 1);
    }

    // multiple consumers
    bool dequeue(int &out) {
        long idx = __sync_fetch_and_add(&consumer_index, 1);
        if(idx >= capacity) return false;
        double start = time_sec();
        while(__sync_val_compare_and_swap(&ready[idx], 1, 1) != 1) {
            if(time_sec() - start > 1.0) do_yield();
        }
        __sync_synchronize();
        out = buf[idx];
        return true;
    }

    bool empty() {
        long c = __sync_add_and_fetch(&consumer_index, 0);
        long p = producer_index; // non-atomic ok since single producer
        return c >= p;
    }
};

/* --------------------- Testing framework --------------------- */

struct Config {
    string test = "combined";
    int producers = 1;
    int consumers = 1;
    int items = 100000;
    int runs = 3;
    string mode = "fixed"; // fixed or random
    int max_threads = get_nprocs() * 2; // rough estimate
} cfg;

static vector<vector<int>> partition_indices(int elements, int parts, bool randomize) {
    vector<int> vals(elements);
    for(int i=0; i<elements; i++) vals[i] = i;
    if(randomize) {
        random_device rd;
        mt19937 g(rd());
        shuffle(vals.begin(), vals.end(), g);
    }
    vector<vector<int>> out(parts);
    for(int i=0; i<elements; i++) out[i % parts].push_back(vals[i]);
    return out;
}

/* OptimisticSet test threads */

struct WriterArgsSet {
    OptimisticSet* s;
    vector<int> items;
};
static void* writer_set_thread(void* arg) {
    WriterArgsSet* a = (WriterArgsSet*)arg;
    for(int v : a->items) a->s->add(v);
    return nullptr;
}

struct ReaderArgsSet {
    OptimisticSet* s;
    vector<int> items;
    vector<int>* out_collected;
};
static void* reader_set_thread(void* arg) {
    ReaderArgsSet* a = (ReaderArgsSet*)arg;
    for(int v : a->items) {
        double start_wait = time_sec();
        while(true) {
            if(a->s->remove(v)) {
                a->out_collected->push_back(v);
                break;
            } else {
                if(time_sec() - start_wait > 1.0) do_yield();
            }
        }
    }
    return nullptr;
}

/* MCSP queue threads */

struct WriterArgsQueue {
    MCSPQueue* q;
    vector<int> items;
};
static void* writer_queue_thread(void* arg) {
    WriterArgsQueue* a = (WriterArgsQueue*)arg;
    for(int v : a->items) a->q->enqueue(v);
    return nullptr;
}

struct ReaderArgsQueue {
    MCSPQueue* q;
    int want_count;
    vector<int>* out_collected;
};
static void* reader_queue_thread(void* arg) {
    ReaderArgsQueue* a = (ReaderArgsQueue*)arg;
    int got = 0;
    double start_wait = time_sec();
    while(got < a->want_count) {
        int val;
        if(a->q->dequeue(val)) {
            a->out_collected->push_back(val);
            ++got;
            start_wait = time_sec();
        } else {
            if(time_sec() - start_wait > 1.0) do_yield();
        }
    }
    return nullptr;
}

/* Combined tests */

bool run_combined_set(int producers, int consumers, int items, bool random, double &elapsed_sec) {
    OptimisticSet s;
    vector<vector<int>> prod_parts = partition_indices(items, producers, random);
    vector<pthread_t> ptids(producers);
    vector<WriterArgsSet> pargs(producers);
    for(int i=0;i<producers;i++){ pargs[i].s = &s; pargs[i].items = prod_parts[i]; }

    vector<vector<int>> read_parts = partition_indices(items, consumers, false); // readers use fixed for verify
    vector<pthread_t> ctids(consumers);
    vector<ReaderArgsSet> cargs(consumers);
    vector<vector<int>> collected(consumers);
    for(int i=0;i<consumers;i++){
        cargs[i].s = &s;
        cargs[i].items = read_parts[i];
        cargs[i].out_collected = &collected[i];
    }

    double t0 = time_sec();
    for(int i=0;i<consumers;i++) pthread_create(&ctids[i], nullptr, reader_set_thread, &cargs[i]);
    for(int i=0;i<producers;i++) pthread_create(&ptids[i], nullptr, writer_set_thread, &pargs[i]);
    for(int i=0;i<producers;i++) pthread_join(ptids[i], nullptr);
    for(int i=0;i<consumers;i++) pthread_join(ctids[i], nullptr);
    double t1 = time_sec();
    elapsed_sec = t1 - t0;

    vector<int> verify(items, 0);
    for(int i=0;i<consumers;i++) for(int v : collected[i]) if(v>=0 && v<items) __sync_fetch_and_add(&verify[v], 1);
    bool ok = true;
    for(int i=0;i<items;i++) if(verify[i] != 1) { ok = false; break; }
    return ok;
}

bool run_combined_queue(int producers, int consumers, int items, bool random, double &elapsed_sec) {
    if(producers != 1) return false; // MCSP
    MCSPQueue q(items);
    vector<vector<int>> prod_parts = partition_indices(items, producers, random);
    vector<pthread_t> ptids(producers);
    vector<WriterArgsQueue> pargs(producers);
    for(int i=0;i<producers;i++){ pargs[i].q = &q; pargs[i].items = prod_parts[i]; }

    int base = items / consumers;
    int rem = items % consumers;
    vector<pthread_t> ctids(consumers);
    vector<ReaderArgsQueue> cargs(consumers);
    vector<vector<int>> collected(consumers);
    for(int i=0;i<consumers;i++){
        cargs[i].q = &q;
        cargs[i].want_count = base + (i < rem ? 1 : 0);
        cargs[i].out_collected = &collected[i];
    }

    double t0 = time_sec();
    for(int i=0;i<consumers;i++) pthread_create(&ctids[i], nullptr, reader_queue_thread, &cargs[i]);
    for(int i=0;i<producers;i++) pthread_create(&ptids[i], nullptr, writer_queue_thread, &pargs[i]);
    for(int i=0;i<producers;i++) pthread_join(ptids[i], nullptr);
    for(int i=0;i<consumers;i++) pthread_join(ctids[i], nullptr);
    double t1 = time_sec();
    elapsed_sec = t1 - t0;

    vector<int> verify(items, 0);
    for(int i=0;i<consumers;i++) for(int v : collected[i]) if(v>=0 && v<items) __sync_fetch_and_add(&verify[v], 1);
    bool ok = true;
    for(int i=0;i<items;i++) if(verify[i] != 1) { ok = false; break; }
    return ok;
}

/* writers/readers-only tests */

bool run_writers_set(int producers, int items, bool random, double &elapsed_sec) {
    double t0 = time_sec();
    OptimisticSet s;
    vector<vector<int>> parts = partition_indices(items, producers, random);
    vector<pthread_t> tids(producers);
    vector<WriterArgsSet> args(producers);
    for(int i=0;i<producers;i++){
        args[i].s = &s; args[i].items = parts[i];
        pthread_create(&tids[i], nullptr, writer_set_thread, &args[i]);
    }
    for(int i=0;i<producers;i++) pthread_join(tids[i], nullptr);
    vector<int> got = s.snapshot();
    sort(got.begin(), got.end());
    elapsed_sec = time_sec() - t0;
    if((int)got.size() != items) return false;
    for(int i=0;i<items;i++) if(got[i] != i) return false;
    return true;
}

bool run_readers_set(int consumers, int items, bool random, double &elapsed_sec) {
    double t0 = time_sec();
    OptimisticSet s;
    vector<int> prefill(items);
    for(int i=0; i<items; i++) prefill[i] = i;
    if(random) {
        random_device rd;
        mt19937 g(rd());
        shuffle(prefill.begin(), prefill.end(), g);
    }
    for(int v : prefill) s.add(v);
    vector<vector<int>> parts = partition_indices(items, consumers, false); // readers fixed
    vector<pthread_t> tids(consumers);
    vector<ReaderArgsSet> args(consumers);
    vector<vector<int>> collected(consumers);
    for(int i=0;i<consumers;i++){
        args[i].s = &s; args[i].items = parts[i]; args[i].out_collected = &collected[i];
        pthread_create(&tids[i], nullptr, reader_set_thread, &args[i]);
    }
    for(int i=0;i<consumers;i++) pthread_join(tids[i], nullptr);
    vector<int> verify(items, 0);
    for(int i=0;i<consumers;i++) for(int v: collected[i]) if(v>=0 && v<items) __sync_fetch_and_add(&verify[v], 1);
    for(int i=0;i<items;i++) if(verify[i] != 1) return false;
    if(!s.snapshot().empty()) return false;
    elapsed_sec = time_sec() - t0;
    return true;
}

bool run_writers_queue(int producers, int items, bool random, double &elapsed_sec) {
    if(producers != 1) return false;
    double t0 = time_sec();
    MCSPQueue q(items);
    vector<vector<int>> parts = partition_indices(items, producers, random);
    vector<pthread_t> tids(producers);
    vector<WriterArgsQueue> args(producers);
    for(int i=0;i<producers;i++){ args[i].q = &q; args[i].items = parts[i]; pthread_create(&tids[i], nullptr, writer_queue_thread, &args[i]); }
    for(int i=0;i<producers;i++) pthread_join(tids[i], nullptr);
    vector<int> got;
    int val;
    while(q.dequeue(val)) got.push_back(val);
    sort(got.begin(), got.end());
    elapsed_sec = time_sec() - t0;
    if((int)got.size() != items) return false;
    for(int i=0;i<items;i++) if(got[i] != i) return false;
    return true;
}

bool run_readers_queue(int consumers, int items, bool random, double &elapsed_sec) {
    double t0 = time_sec();
    MCSPQueue q(items);
    vector<int> prefill(items);
    for(int i=0; i<items; i++) prefill[i] = i;
    if(random) {
        random_device rd;
        mt19937 g(rd());
        shuffle(prefill.begin(), prefill.end(), g);
    }
    for(int v : prefill) q.enqueue(v);
    int base = items / consumers, rem = items % consumers;
    vector<pthread_t> tids(consumers);
    vector<ReaderArgsQueue> args(consumers);
    vector<vector<int>> collected(consumers);
    for(int i=0;i<consumers;i++){
        args[i].q = &q; args[i].want_count = base + (i < rem ? 1 : 0); args[i].out_collected = &collected[i];
        pthread_create(&tids[i], nullptr, reader_queue_thread, &args[i]);
    }
    for(int i=0;i<consumers;i++) pthread_join(tids[i], nullptr);
    vector<int> verify(items, 0);
    for(int i=0;i<consumers;i++) for(int v: collected[i]) if(v>=0 && v<items) __sync_fetch_and_add(&verify[v], 1);
    for(int i=0;i<items;i++) if(verify[i] != 1) return false;
    if(!q.empty()) return false;
    elapsed_sec = time_sec() - t0;
    return true;
}

/* CLI and main */

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -t, --test [writers|readers|combined]  Which test to run (default combined)\n");
    printf("  -p, --producers N             Number of producer threads (default 1)\n");
    printf("  -c, --consumers N             Number of consumer threads (default 1)\n");
    printf("  -n, --items N                 Number of items (default 100000)\n");
    printf("  -r, --runs N                  Number of runs to average (default 3)\n");
    printf("  -m, --mode [fixed|random]     Partition mode for set (default fixed)\n");
    printf("  -h, --help                    Show this help\n");
}

int main(int argc, char** argv) {
    static struct option long_options[] = {
        {"test", required_argument, 0, 't'},
        {"producers", required_argument, 0, 'p'},
        {"consumers", required_argument, 0, 'c'},
        {"items", required_argument, 0, 'n'},
        {"runs", required_argument, 0, 'r'},
        {"mode", required_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while((opt = getopt_long(argc, argv, "t:p:c:n:r:m:h", long_options, nullptr)) != -1) {
        switch(opt) {
            case 't': cfg.test = optarg; break;
            case 'p': cfg.producers = atoi(optarg); break;
            case 'c': cfg.consumers = atoi(optarg); break;
            case 'n': cfg.items = atoi(optarg); break;
            case 'r': cfg.runs = atoi(optarg); break;
            case 'm': cfg.mode = optarg; break;
            case 'h':
            default:
                print_usage(argv[0]); return 0;
        }
    }

    bool randomize = (cfg.mode == "random");
    printf("Test: %s, producers=%d consumers=%d items=%d runs=%d mode=%s max_threads=%d\n",
        cfg.test.c_str(), cfg.producers, cfg.consumers, cfg.items, cfg.runs, cfg.mode.c_str(), cfg.max_threads);

    // For comparative: loop over structures and configs
    vector<string> structures = {"set", "queue"};
    int max_p = 4, max_c = 4; // arbitrary, adjust
    printf("| Producers | Consumers | Structure | Mode | Avg Time (s) | OK |\n");
    printf("|-----------|-----------|-----------|------|--------------|----|\n");

    for(int p=1; p<=max_p; p++) {
        for(int c=1; c<=max_c; c++) {
            if(p + c > cfg.max_threads) continue;
            for(const string& struct_name : structures) {
                if(struct_name == "queue" && p > 1) continue; // MCSP
                vector<double> times;
                bool overall_ok = true;
                for(int run=0; run < cfg.runs; ++run) {
                    double elapsed = 0.0;
                    bool ok = false;
                    if(cfg.test == "combined") {
                        if(struct_name == "set") ok = run_combined_set(p, c, cfg.items, randomize, elapsed);
                        else ok = run_combined_queue(p, c, cfg.items, randomize, elapsed);
                    } else if(cfg.test == "writers") {
                        if(struct_name == "set") ok = run_writers_set(p, cfg.items, randomize, elapsed);
                        else ok = run_writers_queue(p, cfg.items, randomize, elapsed);
                    } else if(cfg.test == "readers") {
                        if(struct_name == "set") ok = run_readers_set(c, cfg.items, randomize, elapsed);
                        else ok = run_readers_queue(c, cfg.items, randomize, elapsed);
                    } else { print_usage(argv[0]); return 1; }
                    times.push_back(elapsed);
                    overall_ok &= ok;
                }
                double avg = 0;
                for(double t: times) avg += t;
                avg /= times.size();
                printf("| %9d | %9d | %9s | %4s | %12.6f | %s |\n",
                    p, c, struct_name.c_str(), cfg.mode.c_str(), avg, overall_ok ? "YES" : "NO");
            }
        }
    }

    return 0;
}