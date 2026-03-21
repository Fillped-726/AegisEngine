
/*Semaphore mutex(1);
Semaphore empty(N);
Semaphore full(0);
queue<int> q;

void producer()
{
    while (true)
    {
        Data d = produce_data();

        empty.wait();
        mutex.wait();
        q.push(d);
        mutex.signal();
        full.signal();
    }
}

void consumer()
{
    while (true)
    {
        full.wait();
        mutex.wait();
        Data d = q.front();
        q.pop();
        mutex.signal();
        empty.signal();

        consumer_data(d);
    }
}*/

#include <semaphore>
#include <queue>

// 定义容量
const int N = 10;
// C++20 计数信号量
std::counting_semaphore<N> empty_sem{N};
std::counting_semaphore<N> full_sem{0};
std::binary_semaphore mutex_sem{1}; // 替代互斥锁

std::queue<int> q;

void producer()
{
    int data = 1;        // 模拟生产
    empty_sem.acquire(); // 等于 wait()
    mutex_sem.acquire();

    q.push(data);

    mutex_sem.release(); // 等于 signal()
    full_sem.release();
}

void consumer()
{
    full_sem.acquire();
    mutex_sem.acquire();

    int data = q.front();
    q.pop();

    mutex_sem.release();
    empty_sem.release();
}