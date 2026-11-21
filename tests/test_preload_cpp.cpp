#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <malloc.h>
#include <new>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace
{

struct alignas(64) Aligned
{
    unsigned char data[128];
};

std::atomic<bool> g_stop{false};
std::atomic<int> g_worker_error{0};

void worker(int id)
{
    unsigned seed = 0x9e3779b9u ^ static_cast<unsigned>(id);
    while (!g_stop.load(std::memory_order_acquire))
    {
        size_t n = 1 + (rand_r(&seed) % 8192);
        unsigned char *p = new (std::nothrow) unsigned char[n];
        if (!p)
        {
            g_worker_error.store(1, std::memory_order_relaxed);
            return;
        }
        p[0] = static_cast<unsigned char>(id);
        p[n - 1] = static_cast<unsigned char>(seed);
        delete[] p;
    }
}

bool check_bytes(const void *p, size_t n, unsigned char v)
{
    const unsigned char *b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i)
    {
        if (b[i] != v)
            return false;
    }
    return true;
}

} // anonymous namespace

int main()
{
    if (!dlsym(RTLD_DEFAULT, "hpm_version"))
    {
        std::println(stderr, "hpmalloc not preloaded");
        return 1;
    }

    const size_t big = 3u * 1024u * 1024u + 123u;
    std::vector<unsigned char> v(big, 0x5a);
    if (v[0] != 0x5a || v[big - 1] != 0x5a)
        return 2;
    for (int i = 0; i < 100000; ++i)
        v.push_back(static_cast<unsigned char>(i));

    std::string s;
    for (int i = 0; i < 10000; ++i)
        s += "abcdefghij";
    if (s.size() != 100000 || s.back() != 'j')
        return 3;

    std::unordered_map<int, std::string> m;
    for (int i = 0; i < 50000; ++i)
        m.emplace(i, std::to_string(i));
    if (m.size() != 50000 || m[49999] != "49999")
        return 4;

    Aligned *a = new Aligned;
    if (reinterpret_cast<unsigned long>(a) % 64 != 0)
        return 5;
    delete a;

    void *nf = operator new(128, std::nothrow);
    if (!nf)
        return 6;
    operator delete(nf, std::nothrow);

    try
    {
        throw std::runtime_error("x");
    }
    catch (const std::exception &e)
    {
        if (std::strcmp(e.what(), "x") != 0)
            return 7;
    }

    void *q = ::operator new(100);
    if (malloc_usable_size(q) < 100)
        return 8;
    ::operator delete(q);

    std::vector<std::thread> workers;
    g_stop.store(false, std::memory_order_release);
    for (int i = 0; i < 4; ++i)
        workers.emplace_back(worker, i);

    std::vector<void *> objects;
    objects.reserve(20000);
    for (int i = 0; i < 20000; ++i)
    {
        size_t n = 1 + static_cast<size_t>(i % 2048);
        void *p = std::malloc(n);
        if (!p)
            return 9;
        std::memset(p, 0x5a, n);
        objects.push_back(p);
    }
    std::vector<std::thread> freers;
    for (int t = 0; t < 4; ++t)
    {
        freers.emplace_back(
            [&objects, t]
            {
                size_t start = static_cast<size_t>(t) * 5000;
                for (size_t i = start; i < start + 5000; ++i)
                {
                    std::free(objects[i]);
                    objects[i] = nullptr;
                }
            });
    }
    for (auto &th : freers)
    {
        if (th.joinable())
            th.join();
    }

    pid_t pid = fork();
    if (pid < 0)
        return 10;
    if (pid == 0)
    {
        for (int i = 0; i < 5000; ++i)
        {
            size_t n = 1 + static_cast<size_t>(i % 4096);
            void *p = std::malloc(n);
            if (!p)
                _exit(20);
            std::memset(p, 0x33, n);
            void *nq = std::realloc(p, n + 64);
            if (!nq)
                _exit(21);
            std::free(nq);
        }
        void *large = std::malloc(4u * 1024u * 1024u);
        if (!large)
            _exit(22);
        std::memset(large, 0x44, 4u * 1024u * 1024u);
        std::free(large);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return 11;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 12;

    g_stop.store(true, std::memory_order_release);
    for (auto &th : workers)
    {
        if (th.joinable())
            th.join();
    }
    if (g_worker_error.load(std::memory_order_acquire))
        return 13;

    void *c = std::calloc(1, 2u * 1024u * 1024u + 17);
    if (!c || !check_bytes(c, 2u * 1024u * 1024u + 17, 0))
        return 14;
    std::free(c);

    std::println("PRELOAD_CPP_PASS");
    return 0;
}
