#include <mpi.h>
#include <deque>
#include <functional>
#include <utility>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <chrono>
#include <thread>
#include <random>

class Message {
    public:
    struct releaseData {
        int city;
        long long timestamp;
    };
    union Data {
        int rank;
        releaseData release;
    };
    enum class Type { REQ, ACK, RELEASE };
    Type type;
    int logicalTimestamp;
    Data data;

    Message() = default;

    static Message req(int logiTime, int source) {
        Message res;
        res.type = Type::REQ;
        res.logicalTimestamp = logiTime;
        res.data.rank = source;
        return res;
    }

    static Message ack(int logiTime, int source) {
        Message res;
        res.type = Type::ACK;
        res.logicalTimestamp = logiTime;
        res.data.rank = source;
        return res;
    }

    static Message release(int logiTime, int city) {
        Message res;
        res.type = Type::ACK;
        res.logicalTimestamp = logiTime;
        res.data.release.city = city;
        res.data.release.timestamp = 
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch() + std::chrono::milliseconds{1000}).count();
        return res;
    }
};

class TrollHunter {
public:

    std::deque<std::pair<int, int>> requestQueue;
    std::vector<unsigned int> releaseCounts;
    std::vector<long long> releaseTimestamp;
    const int size;
    const int rank;
    int logicalClock = 0;
    int ackCount = 0;
    std::mutex Mutex;
    std::condition_variable ackCv;
    std::condition_variable relCv;

    std::mt19937 generator;

    TrollHunter(int Rank, int Size) : rank(Rank), size(Size), generator(std::random_device()()) {}

    void send(Message msg, int dest=-1) {
        if (dest < 0) {
            for (int i = 0; i < size; ++i)
                MPI_Send(&msg, sizeof(Message), MPI_BYTE, i, MPI_ANY_TAG, MPI_COMM_WORLD);
        }
        else {
            assert(dest < size);
            MPI_Send(&msg, sizeof(Message), MPI_BYTE, dest, MPI_ANY_TAG, MPI_COMM_WORLD);
        }
    }

    void insertRequest(int logicalTimestamp, int processId) {
        size_t i = requestQueue.size() - 1;
        while (i > 0 && requestQueue[i].first > logicalTimestamp) --i;
        while (i > 0 && requestQueue[i].first == logicalTimestamp && requestQueue[i].second > processId) --i;
        requestQueue.insert(requestQueue.begin() + i, {logicalTimestamp, processId});
    }

    // size_t getOwnRequestId() {
    //     size_t i = requestQueue.size() - 1;
    //     while (i > 0 && requestQueue[i].second != rank) --i;
    //     return i;
    // }

    size_t getOwnRequestId(){ // im not sure if this is better
        auto it = std::find_if(requestQueue.rbegin(), requestQueue.rend(), 
        [&](const std::pair<int, int> pair) -> bool{
            return pair.second == rank;
        });

        return requestQueue.size() - std::distance(requestQueue.rbegin(),it);
    }


    void listener() {
        MPI_Status status;
        Message msg;
        while (true) {
            MPI_Recv(&msg, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
            std::lock_guard<std::mutex> lock(Mutex);
            logicalClock = std::max(logicalClock, msg.logicalTimestamp) + 1;
            switch (msg.type)
            {
            case Message::Type::REQ:
                insertRequest(msg.logicalTimestamp, msg.data.rank);
                send(Message::ack(logicalClock, rank), msg.data.rank);
                break;
            case Message::Type::ACK:
                ackCount += 1;
                ackCv.notify_all();
                break;
            case Message::Type::RELEASE:
                releaseCounts[msg.data.release.city]++;
                releaseTimestamp[msg.data.release.city] = std::max(releaseTimestamp[msg.data.release.city], msg.data.release.timestamp);
                relCv.notify_all();
                break;
            
            default:
                assert(0);
                break;
            }
        }
    }

    void mainLoop() {
        while (true) {
            std::uniform_real_distribution distrib(1.0, 10.0);
            std::this_thread::sleep_for(std::chrono::duration<double>(distrib(generator)));
            
            std::unique_lock<std::mutex> lock(Mutex);
            logicalClock++;
            insertRequest(logicalClock, rank);
            ackCount = 0;
            send(Message::req(logicalClock, rank));

            ackCv.wait(lock, [&] {return ackCount >= size - 1;});
            // all ack recieved
            size_t index = getOwnRequestId();
            int city = index % size;
            int releasesRequired = index / size;
            
            relCv.wait(lock, [&] {return releaseCounts[city] >= releasesRequired;});

            std::chrono::system_clock::time_point tp{std::chrono::milliseconds{releaseTimestamp[city]}};
            lock.unlock();
            // unlock before waiting
            std::this_thread::sleep_until(tp);
            
            printf("[%d] wchodzę do miasta %d", rank, city);
            std::this_thread::sleep_for(std::chrono::duration<double>(distrib(generator))); // time inside city
            printf("[%d] wychodzę z miasta %d", rank, city);

            lock.lock();
            send(Message::release(logicalClock, city));
            lock.unlock();
        }
    }

};

int main(int argc, char **argv)
{
    // get threading support
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE)
    {
        printf("ERROR: The MPI library does not have full thread support\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    // get rank and size
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    printf("%d / %d\n", rank, size);

    // init troll hunter
    TrollHunter th(rank, size);

    // run troll hunter
    std::thread listenerThread(std::bind(&TrollHunter::listener, &th));
    th.mainLoop();
    listenerThread.join();

    // finish
    MPI_Finalize();
    return 0;
}