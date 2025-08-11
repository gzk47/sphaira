#pragma once

#include "base.hpp"
#include <vector>
#include <memory>
#include <switch.h>

namespace sphaira::yati::container {

struct Xci final : Base {

    struct Partition {
        // name of the partition.
        std::string name;
        // all the collections for this partition, may be empty.
        Collections collections;
    };

    using Partitions = std::vector<Partition>;

    using Base::Base;
    Result GetCollections(Collections& out) override;
    Result GetPartitions(Partitions& out);
};

} // namespace sphaira::yati::container
