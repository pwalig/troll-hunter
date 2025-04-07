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
    // Type type;
    unsigned short type;
    int logicalTimestamp;
    Data data;

    Message() = default;

    static Message req(int logiTime, int source) {
        Message res;
        res.type = static_cast<unsigned short>(Type::REQ);
        res.logicalTimestamp = logiTime;
        res.data.rank = source;
        return res;
    }

    static Message ack(int logiTime, int source) {
        Message res;
        res.type = static_cast<unsigned short>(Type::ACK);
        res.logicalTimestamp = logiTime;
        res.data.rank = source;
        return res;
    }

    static Message release(int logiTime, int city) {
        Message res;
        res.type = static_cast<unsigned short>(Type::RELEASE);
        res.logicalTimestamp = logiTime;
        res.data.release.city = city;
        res.data.release.timestamp = 
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch() + std::chrono::milliseconds{1000}).count();
        return res;
    }

    static MPI_Datatype createMpiType(){
        Message dummy_message;
        MPI_Aint base_addr;
        MPI_Aint displacements[4];

        MPI_Get_address(&dummy_message, &base_addr);
        MPI_Get_address(&dummy_message.type, &displacements[0]);
        MPI_Get_address(&dummy_message.logicalTimestamp, &displacements[1]);
        MPI_Get_address(&dummy_message.data.rank, &displacements[2]);
        MPI_Get_address(&dummy_message.data.release.timestamp, &displacements[3]);
        displacements[0] = MPI_Aint_diff(displacements[0], base_addr);
        displacements[1] = MPI_Aint_diff(displacements[1], base_addr);
        displacements[2] = MPI_Aint_diff(displacements[2], base_addr);
        displacements[3] = MPI_Aint_diff(displacements[3], base_addr);

        MPI_Datatype types[4] = {MPI_UNSIGNED_SHORT, MPI_INT, MPI_INT, MPI_LONG_LONG};
        int lengths[4] = {1,1,1,1};

        MPI_Datatype mpiType;

        MPI_Type_create_struct(4, lengths, displacements, types, &mpiType);
        MPI_Type_commit(&mpiType);

        return mpiType;
    }
};

class TrollHunter {
public:

    const MPI_Datatype msgType;
    std::deque<std::pair<int, int>> requestQueue;
    std::vector<unsigned int> releaseCounts;
    std::vector<long long> releaseTimestamp;
    const int size;
    const int rank;
    const int cityCount;
    int logicalClock = 0;
    int ackCount = 0;
    std::mutex Mutex;
    std::condition_variable ackCv;
    std::condition_variable relCv;

    std::mt19937 generator;

    TrollHunter(int Rank, int Size, const MPI_Datatype msgType, int CityCount) : msgType(msgType), rank(Rank), size(Size), cityCount(CityCount), generator(std::random_device()()), releaseCounts(CityCount), releaseTimestamp(CityCount) {}

    void send(const Message msg, int dest=-1) {
        if (dest < 0) {
            for (int i = 0; i < size; ++i)
                MPI_Send(&msg, 1, msgType, i, 0, MPI_COMM_WORLD);
        }
        else {
            assert(dest < size);
            MPI_Send(&msg, 1, msgType, dest, 0, MPI_COMM_WORLD);
        }
    }

    // void insertRequest(int logicalTimestamp, int processId) {
    //     size_t i = requestQueue.size() - 1;
    //     while (i > 0 && requestQueue[i].first > logicalTimestamp) --i;
    //     while (i > 0 && requestQueue[i].first == logicalTimestamp && requestQueue[i].second > processId) --i;
    //     requestQueue.insert(requestQueue.begin() + i, {logicalTimestamp, processId});
    // }

    void insertRequest(int logicalTimestamp, int processId){
        auto it = std::find_if(requestQueue.rbegin(), requestQueue.rend(), 
        [&](const std::pair<int, int> pair) -> bool{
            return pair.first < logicalTimestamp || (pair.first == logicalTimestamp && pair.second < processId);
        });
        requestQueue.insert(it.base(), {logicalTimestamp, processId});
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

        return std::distance(requestQueue.begin(),it.base()) - 1;
    }


    void listener() {
        MPI_Status status;
        Message msg;
        while (true) {
            MPI_Recv(&msg, 1, msgType, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
            std::lock_guard<std::mutex> lock(Mutex);
            logicalClock = std::max(logicalClock, msg.logicalTimestamp) + 1;
            switch (static_cast<Message::Type>(msg.type))
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
        static std::uniform_real_distribution outOfCityDistrib(1.0, 5.0);
        static std::uniform_real_distribution inCityDistrib(1.0, 5.0);
        while (true) {
            std::this_thread::sleep_for(std::chrono::duration<double>(outOfCityDistrib(generator)));
            
            std::unique_lock<std::mutex> lock(Mutex);
            logicalClock++;

            // dostajemy req od samych siebie więc nie dodajemy requestu tutaj
            // insertRequest(logicalClock, rank);

            ackCount = 0;
            send(Message::req(logicalClock, rank));

            printf("[%d] chcę wejść do miasta\n", rank);
            ackCv.wait(lock, [&] {return ackCount >= size;}); // dostajemy 1 ack od samych siebie

            size_t index = getOwnRequestId();
            int city = index % cityCount;
            int releasesRequired = index / cityCount;
            printf("[%d] zebrałem ack, idx: %ld, chcę wejść do %d, potrzebuję jeszcze %d releasów\n", rank, index, city, releasesRequired - releaseCounts[city]);

            relCv.wait(lock, [&] {return releaseCounts[city] >= releasesRequired;});

            // std::chrono::system_clock::time_point tp{std::chrono::milliseconds{releaseTimestamp[city]}};
            lock.unlock();
            // std::this_thread::sleep_until(tp);
            
            printf("[%d] wchodzę do miasta %d\n", rank, city);
            std::this_thread::sleep_for(std::chrono::duration<double>(inCityDistrib(generator))); // czas w mieście
            printf("[%d] wychodzę z miasta %d\n", rank, city);

            send(Message::release(logicalClock, city)); // no lock needed since MPI_THREAD_MULTIPLE is required
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

    // create type for msg
    MPI_Datatype msgType = Message::createMpiType();

    // init troll hunter
    TrollHunter th(rank, size, msgType, argc > 1 ? std::stoi(argv[1]) : size);

    // run troll hunter
    std::thread listenerThread(std::bind(&TrollHunter::listener, &th));
    th.mainLoop();
    listenerThread.join();

    // finish
    MPI_Finalize();
    return 0;
}