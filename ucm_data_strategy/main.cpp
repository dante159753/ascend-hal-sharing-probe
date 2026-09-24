#include <climits>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include "data_strategy.h"
#include "logger/logger.h"
#include "trans/device.h"

int main(int argc, char** argv)
{
    size_t rank = 0;
    size_t ranks = 2;
    size_t deviceId = 0;
    size_t slotSize = 8ULL << 20;
    size_t slotsPerRank = 256;
    size_t timeoutMs = 600 * 1000;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "Usage: data_strategy_demo [--rank 0] [--ranks 2] [--device 0]\n"
                             "       [--slot-size 8388608] [--slots-per-rank 256]"
                             " [--timeout-ms 600000]\n"
                             "All numbers are decimal; slot-size is bytes. One process per invocation.\n";
                return 0;
            }
            if (++i == argc) { throw std::runtime_error("missing value for " + key); }
            const std::string text = argv[i];
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("expected an unsigned decimal integer for " + key);
            }
            const size_t value = std::stoull(text);
            if (key == "--rank") { rank = value; }
            else if (key == "--ranks") { ranks = value; }
            else if (key == "--device") { deviceId = value; }
            else if (key == "--slot-size") { slotSize = value; }
            else if (key == "--slots-per-rank") { slotsPerRank = value; }
            else if (key == "--timeout-ms") { timeoutMs = value; }
            else { throw std::runtime_error("unknown option: " + key); }
        }
        if (ranks == 0 || ranks > 128 || rank >= ranks || deviceId > INT_MAX || slotSize == 0 ||
            slotsPerRank == 0 || slotsPerRank > std::numeric_limits<size_t>::max() / slotSize ||
            slotsPerRank > std::numeric_limits<size_t>::max() / ranks ||
            timeoutMs > static_cast<size_t>(INT_MAX)) {
            throw std::runtime_error("invalid rank, device, slot dimensions, or timeout");
        }
    } catch (const std::exception& error) {
        std::cerr << "Argument error: " << error.what() << '\n';
        return 2;
    }

    UC::Demo::rank = rank;
    UC::Trans::Device device;
    UC::Status status = device.Init();
    UC_INFO("Device::Init (aclInit): status={}", status);
    if (status.Failure()) { return 1; }

    int result = 0;
    {
        UC::Cache2::CtrlLayout ctrl(ranks, slotsPerRank);
        UC::Cache2::DataStrategy data;
        UC_INFO("DataStrategy::Setup: device={} rank={} ranks={} slot_size={} "
                "slots_per_rank={} timeout_ms={}",
                deviceId, rank, ranks, slotSize, slotsPerRank, timeoutMs);
        status = data.Setup(ctrl, static_cast<int32_t>(deviceId), rank, slotSize, slotsPerRank,
                            timeoutMs);
        UC_INFO("DataStrategy::Setup returned: status={}", status);
        if (status.Failure()) {
            result = 1;
        } else {
            for (size_t peer = 0; peer < ranks; ++peer) {
                const size_t slot = peer * slotsPerRank;
                UC_INFO("  rank={} first_slot={} HostAccessibleOf={} DataAt={} DeviceDataAt={}",
                        peer, slot, data.HostAccessibleOf(slot), data.DataAt(slot),
                        data.DeviceDataAt(slot));
            }
            UC_INFO("SETUP PASS. Keep this process alive until all peers finish Setup; "
                    "then press Enter to release memory and exit.");
            std::string line;
            if (!std::getline(std::cin, line)) {
                UC_ERROR("stdin closed before the lifetime hold was acknowledged");
                result = 1;
            }
        }
    }
    UC_INFO("DataStrategy destroyed; calling aclFinalize");
    status = device.Finalize();
    if (status.Failure()) {
        UC_ERROR("Device::Finalize failed: status={}", status);
        result = 1;
    }
    return result;
}
