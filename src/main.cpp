#include <mpi.h>
#include <deque>
#include <functional>
#include <utility>
#include <vector>
#include <mutex>
#include <cassert>
#include <chrono>
#include <thread>
#include <random>

class Message {
    public:
    struct releaseData {
        int city;
        float timestamp;
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
        // res.data.release.timestamp = std::chrono::system_clock::now();
        return res;
    }
};

class TrollHunter {
public:

    std::deque<std::pair<int, int>> requestQueue;
    std::vector<unsigned int> releaseCounts;
    std::vector<float> releaseTimestamp;
    const int size;
    const int rank;
    int logicalClock = 0;
    int ackCount = 0;
    std::mutex Mutex;
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

    void listener() {
        MPI_Status status;
        Message msg;
        while (true) {
            MPI_Recv(&msg, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
            std::lock_guard<std::mutex> lock(Mutex);
            logicalClock = std::max(logicalClock, msg.logicalTimestamp);
            switch (msg.type)
            {
            case Message::Type::REQ:
                requestQueue.push_back({msg.logicalTimestamp, msg.data.rank}); //TO DO - sort it
                send(Message::ack(logicalClock, rank), msg.data.rank);
                break;
            case Message::Type::ACK:
                ackCount += 1;
                break;
            case Message::Type::RELEASE:
                releaseCounts[msg.data.release.city]++;
                releaseTimestamp[msg.data.release.city] = std::max(releaseTimestamp[msg.data.release.city], msg.data.release.timestamp);
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