#include <Common/HostResolvePool.h>

#include <Common/DNSResolver.h>
#include <Common/Exception.h>
#include <Common/ErrorCodes.h>
#include <Common/thread_local_rng.h>

#include <mutex>

namespace ProfileEvents
{
    extern const Event S3StorageAddressesDiscovered;
    extern const Event S3StorageAddressesExpired;
    extern const Event S3StorageAddressesFailScored;

    extern const Event S3DiskAddressesDiscovered;
    extern const Event S3DiskAddressesExpired;
    extern const Event S3DiskAddressesFailScored;

    extern const Event HttpAddressesDiscovered;
    extern const Event HttpAddressesExpired;
    extern const Event HttpAddressesFailScored;
}

namespace CurrentMetrics
{
    extern const Metric S3StorageAddressesActive;
    extern const Metric S3DiskAddressesActive;
    extern const Metric HttpAddressesActive;
}

namespace DB::ErrorCodes
{
    extern const int DNS_ERROR;
}

DB::HostResolvePoolMetrics getMetricsForS3StorageHostResolve()
{
    return DB::HostResolvePoolMetrics {
        .discovered = ProfileEvents::S3StorageAddressesDiscovered,
        .expired = ProfileEvents::S3StorageAddressesExpired,
        .failed = ProfileEvents::S3StorageAddressesFailScored,
        .active_count = CurrentMetrics::S3StorageAddressesActive,
    };
}

DB::HostResolvePoolMetrics getMetricsForS3DiskHostResolve()
{
    return DB::HostResolvePoolMetrics {
        .discovered = ProfileEvents::S3DiskAddressesDiscovered,
        .expired = ProfileEvents::S3DiskAddressesExpired,
        .failed = ProfileEvents::S3DiskAddressesFailScored,
        .active_count = CurrentMetrics::S3DiskAddressesActive,
    };
}

DB::HostResolvePoolMetrics getMetricsForHttpHostResolve()
{
    return DB::HostResolvePoolMetrics {
        .discovered = ProfileEvents::HttpAddressesDiscovered,
        .expired = ProfileEvents::HttpAddressesExpired,
        .failed = ProfileEvents::HttpAddressesFailScored,
        .active_count = CurrentMetrics::HttpAddressesActive,
    };
}

DB::HostResolvePoolMetrics DB::HostResolvePool::getMetrics(MetricsType type)
{
    switch (type)
    {
        case MetricsType::METRICS_FOR_S3_STORAGE:
            return getMetricsForS3StorageHostResolve();
        case MetricsType::METRICS_FOR_S3_DISK:
            return getMetricsForS3DiskHostResolve();
        case MetricsType::METRICS_FOR_HTTP:
            return getMetricsForHttpHostResolve();
    }
}

DB::HostResolvePool::WeakPtr DB::HostResolvePool::getWeakFromThis()
{
    return weak_from_this();
}

DB::HostResolvePool::HostResolvePool(
    String host_,
    MetricsType metrics_type,
    Poco::Timespan history_)
    : host(std::move(host_))
    , history(history_)
    , metrics(getMetrics(metrics_type))
    , resolve_function([] (const String & host_to_resolve)
    {
      return DB::DNSResolver::instance().resolveHostAll(host_to_resolve);
    })
{
    update();
}

DB::HostResolvePool::HostResolvePool(
    ResolveFunction && resolve_function_,
    MetricsType metrics_type,
    String host_,
    Poco::Timespan history_)
    : host(std::move(host_))
    , history(history_)
    , metrics(getMetrics(metrics_type))
    , resolve_function(std::move(resolve_function_))
{
    update();
}

DB::HostResolvePool::~HostResolvePool()
{
    std::lock_guard lock(mutex);
    CurrentMetrics::sub(metrics.active_count, records.size());
}

void DB::HostResolvePool::Entry::setFail()
{
    if (!fail)
    {
        if (auto lock = pool.lock())
        {
            lock->setFail(address);
        }
    }

    fail = true;
}

DB::HostResolvePool::Entry::~Entry()
{
    if (!fail)
    {
        if (auto lock = pool.lock())
        {
            lock->setSuccess(address);
        }
    }
}

void DB::HostResolvePool::update()
{
    auto next_gen = resolve_function(host);
    if (next_gen.empty())
        throw DB::Exception(ErrorCodes::DNS_ERROR, "no endpoints resolved for host {}", host);

    UpdateStats stats;

    /// upd stats outsize of critical section
    SCOPE_EXIT({
        CurrentMetrics::add(metrics.active_count, stats.added);
        CurrentMetrics::sub(metrics.active_count, stats.expired);
        ProfileEvents::increment(metrics.discovered, stats.added);
        ProfileEvents::increment(metrics.expired, stats.expired);
    });

    Poco::Timestamp now;

    std::lock_guard lock(mutex);
    stats = updateImpl(now, next_gen);
}

void DB::HostResolvePool::updateWeights()
{

    std::lock_guard lock(mutex);
    initWeightMap();
}

DB::HostResolvePool::Entry DB::HostResolvePool::get()
{
    if (isUpdateNeeded())
    {
        update();
    }

    std::lock_guard lock(mutex);
    return Entry(*this, selectBest());
}

void DB::HostResolvePool::setSuccess(const Poco::Net::IPAddress & address)
{
    size_t old_weight = 0;
    size_t new_weight = 0;

    SCOPE_EXIT({
        if (old_weight != new_weight)
        {
            updateWeights();
        }
    });

    std::lock_guard lock(mutex);

    auto it = find(address);
    if (it == records.end())
        return;

    old_weight = it->getWeight();
    ++it->usage;
    new_weight = it->getWeight();

    // cal initWeightMap only when records are updated or setFail
}

void DB::HostResolvePool::setFail(const Poco::Net::IPAddress & address)
{
    Poco::Timestamp now;

    {
        std::lock_guard lock(mutex);

        auto it = find(address);
        if (it == records.end())
            return;
        if (it->fail_bit)
            return;

        it->fail_bit = true;
        it->fail_time = now;
    }

    ProfileEvents::increment(metrics.failed);
    update();
}

Poco::Net::IPAddress DB::HostResolvePool::selectBest()
{
    chassert(!records.empty());
    size_t weight = random_weight_picker(thread_local_rng);
    auto it = std::lower_bound(
        weight_map.begin(), weight_map.end(),
        weight,
        [](const auto & rec, size_t value)
        {
            return rec.first < value;
        });
    chassert(it != weight_map.end());
    return records[it->second].address;
}

DB::HostResolvePool::Records::iterator DB::HostResolvePool::find(const Poco::Net::IPAddress & addr) TSA_REQUIRES(mutex)
{
    return std::lower_bound(
        records.begin(), records.end(),
        addr,
        [](const Record& rec, const Poco::Net::IPAddress & value)
        {
            return rec.address < value;
        });
}

bool DB::HostResolvePool::isUpdateNeeded()
{
        Poco::Timestamp now;

        std::lock_guard lock(mutex);
        return last_resolve_time + history < now || records.empty();
}

DB::HostResolvePool::UpdateStats DB::HostResolvePool::updateImpl(Poco::Timestamp now, std::vector<Poco::Net::IPAddress> & next_gen) TSA_REQUIRES(mutex)
{

    last_resolve_time = now;
    auto last_effective_resolve = last_resolve_time - history;

    UpdateStats stats;

    for (auto & addr : next_gen)
    {
        auto it = find(addr);

        if (it == records.end() || it->address != addr)
        {
            ++stats.added;
            records.insert(it, Record(addr, now));
        }
        else
        {
            ++stats.updated;

            it->resolve_time = now;
            if (it->fail_bit && it->fail_time < last_effective_resolve)
            {
                it->fail_bit = false;
            }
        }
    }

    size_t removed = std::erase_if(
        records,
        [=](const Record & rec)
        {
            return rec.resolve_time < last_effective_resolve;
        });

    stats.expired = removed;

    initWeightMap();

    return stats;
}

void DB::HostResolvePool::initWeightMapImpl()
{
    total_weight = 0;
    weight_map.clear();
    for (size_t i = 0; i < records.size(); ++i)
    {
        auto & rec = records[i];
        if (rec.fail_bit)
            continue;
        total_weight += rec.getWeight();
        weight_map.push_back(std::make_pair(total_weight, i));
    }
}

void DB::HostResolvePool::initWeightMap()
{
    initWeightMapImpl();

    if (total_weight == 0 && !records.empty())
    {
        for (auto & rec: records)
        {
            rec.fail_bit = false;
        }

        initWeightMapImpl();
    }

    chassert(total_weight > 0 && !weight_map.empty() && !records.empty());
    random_weight_picker = std::uniform_int_distribution<size_t>(0, total_weight);
}
