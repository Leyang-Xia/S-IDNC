#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <string>
#include <memory>
#include <atomic>

class Packet {
public:
    Packet(int id, const std::string& content) : id(id), content(content) {}
    int getId() const { return id; }
    std::string getContent() const { return content; }

private:
    int id;
    std::string content;
};

class Sender {
public:
    Sender(int numReceivers) : numReceivers(numReceivers), ackCount(0) {}

    void sendPacket(const std::shared_ptr<Packet>& packet) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            currentPacket = packet;
            ackCount = 0;
            std::cout << "Sender: Sending packet ID " << packet->getId() << " with content: " << packet->getContent() << std::endl;
        }
        cv.notify_all();
    }

    void receiveAck(int packetId) {
        std::unique_lock<std::mutex> lock(mutex);
        if (currentPacket && currentPacket->getId() == packetId) {
            ackCount++;
            std::cout << "Sender: Received Ack for packet ID " << packetId << std::endl;
            if (ackCount == numReceivers) {
                ackReceived.notify_all();
            }
        }
    }

    void waitForAllAcks() {
        std::unique_lock<std::mutex> lock(mutex);
        ackReceived.wait(lock, [this] { return ackCount == numReceivers; });
        std::cout << "Sender: All Acks received for packet ID " << currentPacket->getId() << std::endl;
    }

    std::shared_ptr<Packet> getCurrentPacket() const {
        std::unique_lock<std::mutex> lock(mutex);
        return currentPacket;
    }

private:
    std::shared_ptr<Packet> currentPacket;
    int numReceivers;
    int ackCount;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable ackReceived;
};

class Receiver {
public:
    Receiver(int id, std::shared_ptr<Sender> sender) : id(id), sender(sender) {}

    void operator()() {
        while (true) {
            auto packet = sender->getCurrentPacket();
            if (packet) {
                std::cout << "Receiver " << id << ": Received packet ID " << packet->getId() << " with content: " << packet->getContent() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Simulate packet processing
                sender->receiveAck(packet->getId());
            }
        }
    }

private:
    int id;
    std::shared_ptr<Sender> sender;
};

int main() {
    const int numReceivers = 3;
    auto sender = std::make_shared<Sender>(numReceivers);
    std::vector<std::thread> receiverThreads;

    for (int i = 0; i < numReceivers; ++i) {
        receiverThreads.emplace_back(Receiver(i, sender));
    }

    int packetId = 0;
    while (packetId < 10) {
        auto packet = std::make_shared<Packet>(packetId, "DataPayload" + std::to_string(packetId));
        sender->sendPacket(packet);
        sender->waitForAllAcks();
        packetId++;
    }

    for (auto& t : receiverThreads) {
        if (t.joinable()) {
            t.detach();  // Detach threads since they run indefinitely
        }
    }

    return 0;
}
